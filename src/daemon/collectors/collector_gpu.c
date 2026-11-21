/*
 * collector_gpu.c - GPU 自动探测 + 多显卡负载采集（可切换）
 *
 * 为什么不是“像旧版那样随便给个数字”：
 *   旧版在没有传感器时用 rand() 生成占用率、写死 2048 MB 显存、
 *   用 45 + 占用率/3 编温度，把随机数画成曲线。这不是监控，是装饰。
 *
 * 本模块按“设备身份”和“负载传感器”两条线分别处理：
 *
 *   身份（一定有）：
 *     hw.nvidia.gpus.N.model   NVIDIA 官方型号（含 Laptop 后缀）
 *     dev.drm.N.PCI_ID         DRM 主节点的 PCI 厂商:设备 ID（AMD / Intel）
 *     dev.drm.N.type           0 = 主节点 cardN，2 = 渲染节点 renderDN
 *
 *   负载（能读就读真数据）：
 *     NVIDIA  → nvidia-smi（NVML）
 *     AMD     → libdrm_amdgpu 的 AMDGPU_INFO_SENSOR（负载/温度/频率）
 *               + AMDGPU_INFO_MEMORY（VRAM / GTT 用量）
 *               FreeBSD 的 amdgpu 驱动实现了这套 ioctl，实测可用。
 *     其他    → 只上报设备信息，load_available = 0，界面显示“无传感器”。
 *
 * 这样“核显 + 独显”的笔记本上两张卡都会出现在下拉框里，可随时切换；
 * 有传感器的显示真实曲线，没有传感器的明确说明，不会把“没数据”画成 0%。
 */

#include "fmon_daemon.h"
#include "collector_gpu_nvidia.h"
#include "collector_gpu_internal.h"
#include "collector_gpu_probe.h"
#include "collector_gpu_probe.h"
#include "fmon_sysctl.h"
#include "fmon_child.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef FMON_HAVE_AMDGPU
#include <fcntl.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
#endif

#define GPU_REDETECT_MS 30000   /* 30 秒重新枚举一次，支持驱动延迟加载/热插拔 */



GpuDesc g_descs[FMON_MAX_GPUS];
int     g_desc_count  = 0;
static int64_t g_last_detect = 0;

#ifdef FMON_HAVE_AMDGPU
static struct {
    amdgpu_device_handle dev;
    int                  fd;
} g_amd[FMON_MAX_GPUS];

static void amd_release_all(void)
{
    for (int i = 0; i < FMON_MAX_GPUS; i++) {
        if (g_amd[i].dev != NULL)
            amdgpu_device_deinitialize(g_amd[i].dev);
        if (g_amd[i].fd >= 0)
            close(g_amd[i].fd);
        g_amd[i].dev = NULL;
        g_amd[i].fd  = -1;
    }
}

/* 扫描 renderD*，用 AMDGPU_INFO_DEV_INFO 的 device_id 精确匹配，
 * 不依赖 cardN 与 renderD(128+N) 的编号巧合。 */
static bool amd_open_by_device_id(int desc_index, unsigned device_id)
{
    for (int r = 128; r < 136; r++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", r);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        uint32_t major = 0, minor = 0;
        amdgpu_device_handle dev = NULL;

        if (amdgpu_device_initialize(fd, &major, &minor, &dev) != 0) {
            close(fd);
            continue;
        }

        struct drm_amdgpu_info_device info;
        memset(&info, 0, sizeof(info));

        if (amdgpu_query_info(dev, AMDGPU_INFO_DEV_INFO, sizeof(info), &info) == 0 &&
            info.device_id == device_id) {
            g_amd[desc_index].dev = dev;
            g_amd[desc_index].fd  = fd;
            return true;
        }

        amdgpu_device_deinitialize(dev);
        close(fd);
    }
    return false;
}
#endif /* FMON_HAVE_AMDGPU */

/* ===========================================================================
 * 工具
 * ========================================================================= */

/* rc.d / daemon(8) 启动时 PATH 往往只有 /sbin:/bin:/usr/sbin:/usr/bin，
 * 而 nvidia-smi 在安装前缀（默认 /usr/local）的 bin 下。这里补充的是
 * “搜索目录列表”而不是写死某一个路径：自编译驱动、pkg 安装、NVIDIA
 * 官方目录都能覆盖到。也可以用环境变量 FMON_NVIDIA_SMI 显式指定。 */
/* 在（扩展后的）PATH 里解析出可执行文件的绝对路径。
 * 注意：光“找到”不够 —— execvp 用的是进程自己的 PATH，rc.d 环境下
 * 只有 /sbin:/bin:/usr/sbin:/usr/bin，所以必须用绝对路径去 exec。 */
static void detect_gpus(void)
{
#ifdef FMON_HAVE_AMDGPU
    amd_release_all();
#endif

    PciGpu pci[16];
    int    pci_count = gpu_scan_pciconf(pci, (int)FMON_ARRAY_LEN(pci));

    DrmDevice drm[16];
    int       drm_count = 0;

    g_desc_count = 0;

    /* ---- 1. DRM 主节点（cardN）：说明驱动已加载 ---- */
    for (int i = 0; i < 16 && drm_count < (int)FMON_ARRAY_LEN(drm); i++) {
        char key[64];
        char pci_id[32] = {0};
        int  type       = -1;

        snprintf(key, sizeof(key), "dev.drm.%d.PCI_ID", i);
        if (fmon_sysctl_str(key, pci_id, sizeof(pci_id)) != 0 ||
            pci_id[0] == '\0')
            continue;

        snprintf(key, sizeof(key), "dev.drm.%d.type", i);
        if (fmon_sysctl_int(key, &type) != 0 || type != 0)
            continue;   /* 只统计主节点，跳过 renderD* */

        unsigned vid = 0, did = 0;
        if (sscanf(pci_id, "%x:%x", &vid, &did) != 2)
            continue;

        DrmDevice *dev = &drm[drm_count++];
        strlcpy(dev->pci_id, pci_id, sizeof(dev->pci_id));
        dev->vendor = vid;
        dev->device = did;

        for (int j = 0; j < pci_count; j++) {
            if (!pci[j].used && pci[j].vendor == vid && pci[j].device == did) {
                pci[j].used = true;
                break;
            }
        }
    }

    /* ---- 2. NVIDIA 官方型号（比 PCI 表更准确，含 Laptop 后缀）---- */
    char nv_names[FMON_MAX_GPUS][128];
    int  nv_count = 0;

    for (int i = 0; i < FMON_MAX_GPUS; i++) {
        char key[64];
        char name[128] = {0};

        snprintf(key, sizeof(key), "hw.nvidia.gpus.%d.model", i);
        if (fmon_sysctl_str(key, name, sizeof(name)) != 0 || name[0] == '\0')
            continue;
        strlcpy(nv_names[nv_count++], name, sizeof(nv_names[0]));
    }

    bool have_nvml = gpu_have_binary("nvidia-smi");
    int  nv_used   = 0;

    /* ---- 3. 先处理有 DRM 节点的卡（驱动已加载）---- */
    for (int i = 0; i < drm_count; i++) {
        const DrmDevice *dev = &drm[i];

        const PciGpu *pcidev = NULL;
        for (int j = 0; j < pci_count; j++) {
            if (pci[j].vendor == dev->vendor && pci[j].device == dev->device) {
                pcidev = &pci[j];
                break;
            }
        }

        char     name[64];
        uint32_t kind = FMON_GPU_INTEGRATED;
        const GpuPciName *known = gpu_lookup_pci_name(dev->vendor, dev->device);

        if (dev->vendor == 0x10de && nv_used < nv_count) {
            strlcpy(name, nv_names[nv_used++], sizeof(name));
            kind = FMON_GPU_DISCRETE;
        } else if (known != NULL) {
            strlcpy(name, known->name, sizeof(name));
            kind = known->kind;
        } else if (pcidev != NULL && pcidev->device_name[0] != '\0') {
            snprintf(name, sizeof(name), "%.12s %.48s",
                     gpu_short_vendor(dev->vendor), pcidev->device_name);
            kind = (dev->vendor == 0x8086) ? FMON_GPU_INTEGRATED
                                           : FMON_GPU_DISCRETE;
        } else {
            snprintf(name, sizeof(name), "%s GPU (%s)",
                     gpu_short_vendor(dev->vendor), dev->pci_id);
        }

        uint32_t sensor = FMON_GPU_SENSOR_NONE;
        if (dev->vendor == 0x10de && have_nvml)
            sensor = FMON_GPU_SENSOR_NVML;

        gpu_add_desc(name, gpu_short_vendor(dev->vendor), dev->pci_id, dev->device,
                 kind, sensor, 1);

        if (pcidev != NULL)
            strlcpy(g_descs[g_desc_count - 1].slot, pcidev->slot,
                    sizeof(g_descs[g_desc_count - 1].slot));
    }

    /* ---- 4. NVIDIA 驱动已加载但没有 DRM 节点（例如只装了 nvidia 模块）---- */
    for (int i = nv_used; i < nv_count; i++)
        gpu_add_desc(nv_names[i], "NVIDIA", "", 0, FMON_GPU_DISCRETE,
                 have_nvml ? FMON_GPU_SENSOR_NVML : FMON_GPU_SENSOR_NONE, 1);

    /* ---- 5. pciconf 里有、但没有 DRM 节点的显示设备：驱动未加载 ---- */
    for (int i = 0; i < pci_count; i++) {
        if (pci[i].used)
            continue;

        /* hw.nvidia.gpus 已经给出 NVIDIA 卡；不要再从 vgapci
         * 兜底添加一次，否则同一张卡会出现两个条目。 */
        if (pci[i].vendor == 0x10de && nv_count > 0)
            continue;

        char name[64];
        const GpuPciName *known =
            gpu_lookup_pci_name(pci[i].vendor, pci[i].device);

        if (known != NULL)
            strlcpy(name, known->name, sizeof(name));
        else if (pci[i].device_name[0] != '\0')
            snprintf(name, sizeof(name), "%.12s %.48s",
                     gpu_short_vendor(pci[i].vendor), pci[i].device_name);
        else
            snprintf(name, sizeof(name), "%s GPU (%04x:%04x)",
                     gpu_short_vendor(pci[i].vendor), pci[i].vendor, pci[i].device);

        if (!gpu_add_desc(name, gpu_short_vendor(pci[i].vendor), "", pci[i].device,
                      FMON_GPU_DISCRETE, FMON_GPU_SENSOR_NONE, 0))
            break;

        strlcpy(g_descs[g_desc_count - 1].slot, pci[i].slot,
                sizeof(g_descs[g_desc_count - 1].slot));
    }

#ifdef FMON_HAVE_AMDGPU
    /* ---- 6. 给 AMD 卡接上 libdrm_amdgpu 传感器 ---- */
    for (int i = 0; i < g_desc_count; i++) {
        if (strcmp(g_descs[i].vendor, "AMD") != 0 ||
            g_descs[i].pci_device == 0 || !g_descs[i].driver_loaded)
            continue;
        if (amd_open_by_device_id(i, g_descs[i].pci_device))
            g_descs[i].sensor = FMON_GPU_SENSOR_DRM;
    }
#endif
}

/* ===========================================================================
 * 负载采样
 * ========================================================================= */

/* 从 amdgpu.ko 的构建路径字符串里提取 drm-kmod 版本（如 v6.12.110）。
 * 自编译驱动没有 pkg 元数据，这是最可靠的版本来源。
 * 注意 .ko 是二进制，必须用 memmem 而不是 strstr 搜索。 */
static void detect_amd_driver_version(char *out, size_t out_len)
{
    static const char *const paths[] = {
        "/boot/modules/amdgpu.ko",
        "/boot/kernel/amdgpu.ko",
    };

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *fp = fopen(paths[i], "rb");

        if (fp == NULL)
            continue;

        char   buf[262144];
        size_t keep = 0;
        bool   found = false;

        while (!found) {
            size_t n = fread(buf + keep, 1, sizeof(buf) - 1 - keep, fp);

            if (n == 0)
                break;

            size_t total = keep + n;

            const char *p = memmem(buf, total, "drm_v", 5);

            if (p != NULL) {
                const char *q = p + 5;
                const char *limit = buf + total;

                char   ver[24];
                size_t k = 0;

                while (q + k < limit && k < sizeof(ver) - 1 &&
                       ((q[k] >= '0' && q[k] <= '9') || q[k] == '.'))
                    k++;

                if (k > 0) {
                    memcpy(ver, q, k);
                    ver[k] = '\0';
                    snprintf(out, out_len, "drm-kmod v%s", ver);
                    found = true;
                    break;
                }
            }

            keep = (total > 16) ? 16 : total;
            memmove(buf, buf + total - keep, keep);
        }

        fclose(fp);

        if (found)
            return;
    }

    strlcpy(out, "amdgpu (FreeBSD DRM)", out_len);
}

/* AMD 卡从 FreeBSD DRM sysctl 读取固件/唯一标识。
 * 用 AMD 专有的 fw_version.smc_fw_version 节点识别 amdgpu 设备，
 * 避免把 NVIDIA 的 drmnN 误当成 AMD。 */
static void fill_amd_meta(GpuDesc *d)
{
    for (int n = 0; n < 10; n++) {
        char   key[80];
        char   val[128];
        size_t len;

        snprintf(key, sizeof(key), "sys.device.drmn%d.fw_version.smc_fw_version",
                 n);
        len = sizeof(val);

        if (sysctlbyname(key, val, &len, NULL, 0) != 0)
            continue;

        detect_amd_driver_version(d->driver, sizeof(d->driver));

        snprintf(key, sizeof(key), "sys.device.drmn%d.vbios_version", n);
        len = sizeof(val);
        if (sysctlbyname(key, val, &len, NULL, 0) == 0)
            strlcpy(d->vbios, val, sizeof(d->vbios));

        snprintf(key, sizeof(key), "sys.device.drmn%d.unique_id", n);
        len = sizeof(val);
        if (sysctlbyname(key, val, &len, NULL, 0) == 0 && val[0] != '\0')
            strlcpy(d->uuid, val, sizeof(d->uuid));

        return;
    }
}

static void fill_base(FmonShmData *s, int n)
{
    for (int i = 0; i < n; i++) {
        FmonGpuInfo   *g = &s->gpus[i];
        const GpuDesc *d = &g_descs[i];

        memset(g, 0, sizeof(*g));
        strlcpy(g->name, d->name, sizeof(g->name));
        strlcpy(g->vendor, d->vendor, sizeof(g->vendor));
        strlcpy(g->pci_id, d->pci_id, sizeof(g->pci_id));
        strlcpy(g->slot, d->slot, sizeof(g->slot));

        if (strcmp(d->vendor, "AMD") == 0 && d->driver[0] == '\0')
            fill_amd_meta(&g_descs[i]);

        strlcpy(g->driver_version, d->driver, sizeof(g->driver_version));
        strlcpy(g->vbios_version, d->vbios, sizeof(g->vbios_version));
        strlcpy(g->serial, d->serial, sizeof(g->serial));
        strlcpy(g->uuid, d->uuid, sizeof(g->uuid));

        g->present        = 1;
        g->kind           = d->kind;
        g->sensor         = d->sensor;
        g->driver_loaded  = d->driver_loaded;
        g->load_available = 0;
        g->usage_pct      = -1.0;
        g->temp_c         = -1.0;
    }
}

/* CSV 字段：返回去掉首尾空格的 token；"[N/A]" 视为未知 */
char *gpu_csv_next(char **save, char *buf, size_t len)
{
    char *p = *save;

    if (p == NULL)
        return NULL;

    char *comma = strchr(p, ',');

    if (comma != NULL)
        *comma = '\0';

    while (*p == ' ')
        p++;

    strlcpy(buf, p, len);

    size_t n = strlen(buf);

    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\r'))
        buf[--n] = '\0';

    *save = (comma != NULL) ? (comma + 1) : NULL;
    return buf;
}

#ifdef FMON_HAVE_AMDGPU
static void sample_amd(FmonShmData *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (g_descs[i].sensor != FMON_GPU_SENSOR_DRM || g_amd[i].dev == NULL)
            continue;

        FmonGpuInfo *g   = &s->gpus[i];
        amdgpu_device_handle dev = g_amd[i].dev;

        uint32_t load = 0, temp = 0, sclk = 0;

        if (amdgpu_query_sensor_info(dev, AMDGPU_INFO_SENSOR_GPU_LOAD, 4,
                                     &load) == 0) {
            g->usage_pct = (double)load;
            g->load_available = 1;
        }

        if (amdgpu_query_sensor_info(dev, AMDGPU_INFO_SENSOR_GPU_TEMP, 4,
                                     &temp) == 0 && temp > 0)
            g->temp_c = (double)temp / 1000.0;

        if (amdgpu_query_sensor_info(dev, AMDGPU_INFO_SENSOR_GFX_SCLK, 4,
                                     &sclk) == 0)
            g->clock_mhz = sclk;

        struct drm_amdgpu_memory_info mi;
        memset(&mi, 0, sizeof(mi));

        if (amdgpu_query_info(dev, AMDGPU_INFO_MEMORY, sizeof(mi), &mi) == 0) {
            /* 分开上报：VRAM（专用显存）与 GTT（共享系统内存）。
             * 核显两栏都有，界面分别标注；独显 GTT 通常为 0。 */
            {
                g->mem_used_mb  = (double)mi.vram.heap_usage / 1048576.0;
                g->mem_total_mb = (double)mi.vram.total_heap_size / 1048576.0;
                g->gtt_used_mb  = (double)mi.gtt.heap_usage / 1048576.0;
                g->gtt_total_mb = (double)mi.gtt.total_heap_size / 1048576.0;
            }
        }
    }
}
#endif /* FMON_HAVE_AMDGPU */

/* ===========================================================================
 * 对外接口
 *
 * 由 thread_io 每 5 秒调用一次。内部自己加写锁：nvidia-smi / ioctl 可能
 * 耗时数百毫秒，不能把它们放进别的写事务里，否则会拖慢发布线程的心跳。
 * ========================================================================= */
void fmon_gpu_collect(FmonShmData *s)
{
    int64_t now = fmon_now_ms();

    if (g_desc_count == 0 || now - g_last_detect >= GPU_REDETECT_MS) {
        detect_gpus();
        g_last_detect = now;
    }

    int n = g_desc_count;
    if (n < 0)
        n = 0;
    if (n > FMON_MAX_GPUS)
        n = FMON_MAX_GPUS;

    bool need_sensor = false;
    for (int i = 0; i < n; i++) {
        if (g_descs[i].sensor != FMON_GPU_SENSOR_NONE)
            need_sensor = true;
    }

    if (need_sensor) {
        /* 在私有缓冲里完成慢查询，最后一次性、短暂地写入共享内存 */
        FmonShmData *tmp = calloc(1, sizeof(*tmp));

        if (tmp != NULL) {
            fill_base(tmp, n);
            gpu_sample_nvml(tmp, n);
#ifdef FMON_HAVE_AMDGPU
            sample_amd(tmp, n);
#endif

            fmon_shm_wr_begin();
            for (int i = 0; i < n; i++)
                s->gpus[i] = tmp->gpus[i];
            s->gpu_count = n;
            fmon_shm_wr_end();

            free(tmp);
            return;
        }
    }

    fmon_shm_wr_begin();
    fill_base(s, n);
    s->gpu_count = n;
    fmon_shm_wr_end();
}
