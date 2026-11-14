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

typedef struct {
    char     name[64];
    char     vendor[16];
    char     pci_id[16];
    char     slot[24];        /* PCI 槽位，如 pci0:1:0:0（来自 pciconf）*/
    unsigned pci_vendor;
    unsigned pci_device;
    uint32_t kind;
    uint32_t sensor;
    uint32_t driver_loaded;   /* 0 = 只探测到硬件，驱动未加载 */
    char     driver[48];      /* 驱动版本描述 */
    char     vbios[48];       /* 固件版本 */
    char     serial[64];
    char     uuid[64];
} GpuDesc;

/* PCI 设备名表：命中给可读型号，未命中回退为 "AMD GPU (1002:xxxx)" */
typedef struct {
    uint16_t    vendor;
    uint16_t    device;
    const char *name;
    uint32_t    kind;
} GpuPciName;

static const GpuPciName k_gpu_names[] = {
    /* ---- AMD APU / 核显 ---- */
    { 0x1002, 0x15bf, "AMD Radeon 780M Graphics",    FMON_GPU_INTEGRATED },
    { 0x1002, 0x15c8, "AMD Radeon 760M Graphics",    FMON_GPU_INTEGRATED },
    { 0x1002, 0x15d8, "AMD Radeon Vega 8 Graphics",  FMON_GPU_INTEGRATED },
    { 0x1002, 0x1638, "AMD Radeon 660M Graphics",    FMON_GPU_INTEGRATED },
    { 0x1002, 0x164e, "AMD Radeon 660M Graphics",    FMON_GPU_INTEGRATED },
    { 0x1002, 0x1681, "AMD Radeon 680M Graphics",    FMON_GPU_INTEGRATED },
    { 0x1002, 0x1682, "AMD Radeon 680M Graphics",    FMON_GPU_INTEGRATED },
    { 0x1002, 0x13c6, "AMD Radeon Vega 8 Graphics",  FMON_GPU_INTEGRATED },
    /* ---- AMD 独显 ---- */
    { 0x1002, 0x73df, "AMD Radeon RX 6700 XT",       FMON_GPU_DISCRETE },
    { 0x1002, 0x73ff, "AMD Radeon RX 6600",          FMON_GPU_DISCRETE },
    { 0x1002, 0x744c, "AMD Radeon RX 7900 XTX",      FMON_GPU_DISCRETE },
    { 0x1002, 0x747e, "AMD Radeon RX 7800 XT",       FMON_GPU_DISCRETE },
    /* ---- Intel 核显（FreeBSD 目前没有对应的负载接口）---- */
    { 0x8086, 0x46a6, "Intel Iris Xe Graphics",      FMON_GPU_INTEGRATED },
    { 0x8086, 0x9a49, "Intel Iris Xe Graphics",      FMON_GPU_INTEGRATED },
    { 0x8086, 0xa7a0, "Intel Iris Xe Graphics",      FMON_GPU_INTEGRATED },
    { 0x8086, 0x7d55, "Intel Arc Graphics",          FMON_GPU_INTEGRATED },
    { 0x8086, 0x3e9b, "Intel UHD Graphics 630",      FMON_GPU_INTEGRATED },
    { 0x8086, 0x1916, "Intel HD Graphics 520",       FMON_GPU_INTEGRATED },
    /* ---- NVIDIA 兜底（通常由 hw.nvidia 提供更准确的型号）---- */
    { 0x10de, 0x28e0, "NVIDIA GeForce RTX 4060 Laptop GPU", FMON_GPU_DISCRETE },
    { 0x10de, 0x2882, "NVIDIA GeForce RTX 4060",     FMON_GPU_DISCRETE },
    { 0x10de, 0x25a2, "NVIDIA GeForce RTX 3060 Laptop GPU", FMON_GPU_DISCRETE },
    { 0x10de, 0x2504, "NVIDIA GeForce RTX 3060",     FMON_GPU_DISCRETE },
};

static GpuDesc g_descs[FMON_MAX_GPUS];
static int     g_desc_count  = 0;
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
#define FMON_DEFAULT_PATH FMON_LOCALBASE "/bin:" FMON_LOCALBASE "/sbin:" \
                          "/usr/bin:/bin:/usr/sbin:/sbin:" \
                          "/usr/local/nvidia/bin:/opt/bin"

/* 在（扩展后的）PATH 里解析出可执行文件的绝对路径。
 * 注意：光“找到”不够 —— execvp 用的是进程自己的 PATH，rc.d 环境下
 * 只有 /sbin:/bin:/usr/sbin:/usr/bin，所以必须用绝对路径去 exec。 */
static bool resolve_binary(const char *name, char *out, size_t out_len)
{
    const char *env = getenv("PATH");
    char   path[1024];

    if (env != NULL && env[0] != '\0')
        snprintf(path, sizeof(path), "%s:%s", env, FMON_DEFAULT_PATH);
    else
        snprintf(path, sizeof(path), "%s", FMON_DEFAULT_PATH);

    size_t      name_len = strlen(name);
    const char *p        = path;

    while (*p != '\0') {
        const char *sep = strchr(p, ':');
        size_t      len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);

        if (len > 0 && len + 1 + name_len < out_len) {
            memcpy(out, p, len);
            out[len] = '/';
            memcpy(out + len + 1, name, name_len + 1);
            if (access(out, X_OK) == 0)
                return true;
        }

        if (sep == NULL)
            break;
        p = sep + 1;
    }

    out[0] = '\0';
    return false;
}

static bool have_binary(const char *name)
{
    char buf[1024];

    return resolve_binary(name, buf, sizeof(buf));
}

static const GpuPciName *lookup_pci_name(unsigned vendor, unsigned device)
{
    for (size_t i = 0; i < FMON_ARRAY_LEN(k_gpu_names); i++) {
        if (k_gpu_names[i].vendor == vendor && k_gpu_names[i].device == device)
            return &k_gpu_names[i];
    }
    return NULL;
}

static bool add_desc(const char *name, const char *vendor, const char *pci_id,
                     unsigned pci_device, uint32_t kind, uint32_t sensor,
                     uint32_t driver_loaded)
{
    if (g_desc_count >= FMON_MAX_GPUS)
        return false;

    GpuDesc *d = &g_descs[g_desc_count++];

    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    strlcpy(d->vendor, vendor, sizeof(d->vendor));
    strlcpy(d->pci_id, (pci_id != NULL) ? pci_id : "", sizeof(d->pci_id));
    d->pci_device    = pci_device;
    d->kind          = kind;
    d->sensor        = sensor;
    d->driver_loaded = driver_loaded;
    return true;
}

/* ===========================================================================
 * 设备枚举
 * ========================================================================= */
typedef struct {
    char     slot[24];
    unsigned vendor;
    unsigned device;
    char     vendor_name[64];
    char     device_name[96];
    bool     used;          /* 已经与 DRM 节点配对 */
} PciGpu;

typedef struct {
    char     pci_id[16];
    unsigned vendor;
    unsigned device;
} DrmDevice;

/* 从 pciconf -lv 里解析所有显示类设备（class 0x030000 VGA / 0x030200 3D）。
 * 好处：即使驱动没加载也能拿到型号名（来自 PCI 厂商数据库），
 * 而 DRM 节点只有在驱动加载后才存在。 */
static int scan_pciconf(PciGpu *out, int max)
{
    const char *const argv[] = { "pciconf", "-lv", NULL };

    FmonChildResult res;
    if (fmon_child_run(argv, 512 * 1024, 4000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return 0;
    }

    int    n   = 0;
    PciGpu *cur = NULL;
    char  *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        if (strncmp(line, "vgapci", 6) == 0 && strchr(line, '@') != NULL) {
            cur = NULL;

            bool display = (strstr(line, "class=0x030000") != NULL ||
                            strstr(line, "class=0x030200") != NULL);
            if (!display || n >= max)
                continue;

            PciGpu *d = &out[n];
            memset(d, 0, sizeof(*d));

            /* 槽位：vgapci0@pci0:1:0:0: → "pci0:1:0:0"
             * 取 @ 之后到空白之前，再去掉结尾的冒号。 */
            const char *at    = strchr(line, '@') + 1;
            const char *end   = at;

            while (*end != '\0' && *end != ' ' && *end != '\t')
                end++;

            size_t len = (size_t)(end - at);
            if (len > 0 && at[len - 1] == ':')
                len--;
            if (len >= sizeof(d->slot))
                len = sizeof(d->slot) - 1;
            memcpy(d->slot, at, len);
            d->slot[len] = '\0';

            const char *v = strstr(line, "vendor=0x");
            const char *p = strstr(line, "device=0x");
            if (v != NULL)
                (void)sscanf(v, "vendor=0x%x", &d->vendor);
            if (p != NULL)
                (void)sscanf(p, "device=0x%x", &d->device);

            cur = d;
            n++;
            continue;
        }

        if (cur == NULL)
            continue;

        /* 缩进行的 "vendor = '...'" / "device = '...'" */
        const char *q1 = strchr(line, '\'');
        const char *q2 = (q1 != NULL) ? strchr(q1 + 1, '\'') : NULL;
        if (q1 == NULL || q2 == NULL)
            continue;

        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        size_t vlen = (size_t)(q2 - q1 - 1);

        if (strncmp(p, "vendor", 6) == 0) {
            if (vlen >= sizeof(cur->vendor_name))
                vlen = sizeof(cur->vendor_name) - 1;
            memcpy(cur->vendor_name, q1 + 1, vlen);
            cur->vendor_name[vlen] = '\0';
        } else if (strncmp(p, "device", 6) == 0) {
            if (vlen >= sizeof(cur->device_name))
                vlen = sizeof(cur->device_name) - 1;
            memcpy(cur->device_name, q1 + 1, vlen);
            cur->device_name[vlen] = '\0';
        }
    }

    fmon_child_result_free(&res);
    return n;
}

static const char *short_vendor(unsigned vid)
{
    switch (vid) {
    case 0x10de: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    default:     return "Unknown";
    }
}

static void detect_gpus(void)
{
#ifdef FMON_HAVE_AMDGPU
    amd_release_all();
#endif

    PciGpu pci[16];
    int    pci_count = scan_pciconf(pci, (int)FMON_ARRAY_LEN(pci));

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

    bool have_nvml = have_binary("nvidia-smi");
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
        const GpuPciName *known = lookup_pci_name(dev->vendor, dev->device);

        if (dev->vendor == 0x10de && nv_used < nv_count) {
            strlcpy(name, nv_names[nv_used++], sizeof(name));
            kind = FMON_GPU_DISCRETE;
        } else if (known != NULL) {
            strlcpy(name, known->name, sizeof(name));
            kind = known->kind;
        } else if (pcidev != NULL && pcidev->device_name[0] != '\0') {
            snprintf(name, sizeof(name), "%.12s %.48s",
                     short_vendor(dev->vendor), pcidev->device_name);
            kind = (dev->vendor == 0x8086) ? FMON_GPU_INTEGRATED
                                           : FMON_GPU_DISCRETE;
        } else {
            snprintf(name, sizeof(name), "%s GPU (%s)",
                     short_vendor(dev->vendor), dev->pci_id);
        }

        uint32_t sensor = FMON_GPU_SENSOR_NONE;
        if (dev->vendor == 0x10de && have_nvml)
            sensor = FMON_GPU_SENSOR_NVML;

        add_desc(name, short_vendor(dev->vendor), dev->pci_id, dev->device,
                 kind, sensor, 1);

        if (pcidev != NULL)
            strlcpy(g_descs[g_desc_count - 1].slot, pcidev->slot,
                    sizeof(g_descs[g_desc_count - 1].slot));
    }

    /* ---- 4. NVIDIA 驱动已加载但没有 DRM 节点（例如只装了 nvidia 模块）---- */
    for (int i = nv_used; i < nv_count; i++)
        add_desc(nv_names[i], "NVIDIA", "", 0, FMON_GPU_DISCRETE,
                 have_nvml ? FMON_GPU_SENSOR_NVML : FMON_GPU_SENSOR_NONE, 1);

    /* ---- 5. pciconf 里有、但没有 DRM 节点的显示设备：驱动未加载 ---- */
    for (int i = 0; i < pci_count; i++) {
        if (pci[i].used)
            continue;

        char name[64];

        if (pci[i].device_name[0] != '\0')
            snprintf(name, sizeof(name), "%.12s %.48s",
                     short_vendor(pci[i].vendor), pci[i].device_name);
        else
            snprintf(name, sizeof(name), "%s GPU (%04x:%04x)",
                     short_vendor(pci[i].vendor), pci[i].vendor, pci[i].device);

        if (!add_desc(name, short_vendor(pci[i].vendor), "", pci[i].device,
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
static char *csv_next(char **save, char *buf, size_t len)
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

static double csv_number(const char *s)
{
    if (s == NULL || s[0] == '\0' || s[0] == '[')
        return -1.0;

    return strtod(s, NULL);
}

/* nvidia-smi -q 的补充明细：BAR1 / ECC / P-State */
static void parse_nvml_q(const char *out, FmonGpuInfo **gpus, int nv_count)
{
    char *copy = strdup(out);
    char *save = NULL;
    int   ord  = -1;
    int   section = 0;   /* 1=BAR1 2=ECC */

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        while (*line == ' ' || *line == '\t')
            line++;

        if (strncmp(line, "GPU 0000", 8) == 0) {
            ord++;
            section = 0;
            continue;
        }

        if (ord < 0 || ord >= nv_count)
            continue;

        FmonGpuInfo *g = gpus[ord];

        if (strncmp(line, "BAR1 Memory Usage", 17) == 0) {
            section = 1;
            continue;
        }
        if (strncmp(line, "ECC Mode", 8) == 0) {
            section = 2;
            continue;
        }
        if (strncmp(line, "Performance State", 17) == 0) {
            char st[16] = {0};

            if (sscanf(line, "Performance State : P%15s", st) == 1)
                g->pstate = (uint32_t)strtoul(st, NULL, 10);
            section = 0;
            continue;
        }
        if (strncmp(line, "Single Bit ECC", 14) == 0) {
            char v[32] = {0};

            if (sscanf(line, "Single Bit ECC : %31s", v) == 1 && v[0] != 'N')
                g->ecc_sbe = strtoull(v, NULL, 10);
            continue;
        }
        if (strncmp(line, "Double Bit ECC", 14) == 0) {
            char v[32] = {0};

            if (sscanf(line, "Double Bit ECC : %31s", v) == 1 && v[0] != 'N')
                g->ecc_dbe = strtoull(v, NULL, 10);
            continue;
        }

        if (section == 1) {
            char *colon = strchr(line, ':');

            if (colon != NULL) {
                double mb = strtod(colon + 1, NULL);

                if (strncmp(line, "Total", 5) == 0)
                    g->bar1_total_mb = mb;
                else if (strncmp(line, "Used", 4) == 0)
                    g->bar1_used_mb = mb;
            }
        } else if (section == 2) {
            if (strncmp(line, "Current", 7) == 0) {
                if (strstr(line, "Enabled") != NULL)
                    g->ecc_mode = 2;
                else if (strstr(line, "Disabled") != NULL)
                    g->ecc_mode = 1;
            }
        }
    }

    free(copy);
}

/* nvidia-smi dmon -s t：实时 PCIe 吞吐（MB/s） */
static void parse_nvml_pcie(const char *out, FmonGpuInfo **gpus, int nv_count)
{
    char *copy = strdup(out);
    char *save = NULL;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        if (line[0] == '#')
            continue;

        int    idx = -1;
        double rx = 0.0, tx = 0.0;

        if (sscanf(line, " %d %lf %lf", &idx, &rx, &tx) == 3 && idx >= 0 &&
            idx < nv_count) {
            gpus[idx]->pcie_rx_mbps = rx;
            gpus[idx]->pcie_tx_mbps = tx;
        }
    }

    free(copy);
}

static void sample_nvml(FmonShmData *s, int n)
{
    /* 必须用绝对路径：rc.d 环境下进程的 PATH 不含 /usr/local/bin，
     * 直接 execvp("nvidia-smi") 会得到 127（找不到文件）。
     * 支持 FMON_NVIDIA_SMI 覆盖（自编译驱动可放在任意位置）。 */
    static char nvsmi[1024];

    if (nvsmi[0] == '\0') {
        const char *env = getenv("FMON_NVIDIA_SMI");

        if (env != NULL && env[0] != '\0' && access(env, X_OK) == 0)
            strlcpy(nvsmi, env, sizeof(nvsmi));
        else if (!resolve_binary("nvidia-smi", nvsmi, sizeof(nvsmi)))
            return;
    }

    const char *argv[] = {
        nvsmi,
        "--query-gpu=name,driver_version,vbios_version,serial,uuid,"
        "memory.total,memory.used,memory.free,"
        "utilization.gpu,utilization.memory,utilization.encoder,"
        "utilization.decoder,utilization.jpeg,utilization.ofa,"
        "clocks.current.graphics,clocks.current.sm,clocks.current.memory,"
        "clocks.current.video,temperature.gpu,fan.speed,power.draw,"
        "power.limit,pcie.link.gen.current,pcie.link.width.current",
        "--format=csv,noheader,nounits",
        NULL
    };

    FmonChildResult res;
    if (fmon_child_run(argv, 128 * 1024, 3000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return;   /* 保持 load_available = 0 */
    }

    bool filled[FMON_MAX_GPUS] = { false };
    int  line_no = 0;
    char *save   = NULL;

    for (char *line = strtok_r(res.out, "\n", &save);
         line != NULL && line_no < n;
         line = strtok_r(NULL, "\n", &save)) {

        char *pos = line;
        char  f[24][64];
        int   got = 0;

        while (got < 24) {
            if (csv_next(&pos, f[got], sizeof(f[got])) == NULL)
                break;
            got++;
        }

        if (got < 5)
            continue;

        char *name = f[0];

        int idx = -1;

        for (int j = 0; j < n; j++) {
            if (!filled[j] && g_descs[j].sensor == FMON_GPU_SENSOR_NVML &&
                strcmp(g_descs[j].name, name) == 0) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            for (int j = 0; j < n; j++) {
                if (!filled[j] && g_descs[j].sensor == FMON_GPU_SENSOR_NVML) {
                    idx = j;
                    break;
                }
            }
        }
        if (idx < 0)
            continue;

        FmonGpuInfo *g = &s->gpus[idx];

        g->load_available   = 1;
        g->usage_pct        = csv_number(f[8]);
        g->mem_used_mb      = csv_number(f[6]);
        g->mem_total_mb     = csv_number(f[5]);
        g->temp_c           = csv_number(f[18]);
        g->clock_mhz        = (uint32_t)((csv_number(f[14]) > 0.0)
                                             ? csv_number(f[14])
                                             : 0.0);

        if (f[1][0] != '[')
            strlcpy(g->driver_version, f[1], sizeof(g->driver_version));
        if (f[2][0] != '[')
            strlcpy(g->vbios_version, f[2], sizeof(g->vbios_version));
        if (f[3][0] != '[')
            strlcpy(g->serial, f[3], sizeof(g->serial));
        if (f[4][0] != '[')
            strlcpy(g->uuid, f[4], sizeof(g->uuid));

        g->util_mem_pct    = csv_number(f[9]);
        g->util_enc_pct    = csv_number(f[10]);
        g->util_dec_pct    = csv_number(f[11]);
        g->util_jpeg_pct   = csv_number(f[12]);
        g->util_ofa_pct    = csv_number(f[13]);
        g->clock_sm_mhz    = csv_number(f[15]);
        g->clock_mem_mhz   = csv_number(f[16]);
        g->clock_video_mhz = csv_number(f[17]);
        g->fan_pct         = csv_number(f[19]);
        g->power_w         = csv_number(f[20]);
        g->power_limit_w   = csv_number(f[21]);
        g->pcie_gen        = (uint32_t)((csv_number(f[22]) > 0.0)
                                            ? csv_number(f[22])
                                            : 0.0);
        g->pcie_width      = (uint32_t)((csv_number(f[23]) > 0.0)
                                            ? csv_number(f[23])
                                            : 0.0);

        filled[idx] = true;
        line_no++;
    }

    fmon_child_result_free(&res);

    /* 每 10 秒补充 BAR1 / ECC / PCIe 吞吐 */
    static int64_t next_detail_ms = 0;
    int64_t        now = fmon_now_ms();

    if (now < next_detail_ms)
        return;

    next_detail_ms = now + 10000;

    FmonGpuInfo *nv[FMON_MAX_GPUS];
    int          nv_count = 0;

    for (int j = 0; j < n && nv_count < FMON_MAX_GPUS; j++) {
        if (g_descs[j].sensor == FMON_GPU_SENSOR_NVML)
            nv[nv_count++] = &s->gpus[j];
    }

    if (nv_count == 0)
        return;

    const char *qargv[] = { nvsmi, "-q", NULL };
    FmonChildResult qres;

    if (fmon_child_run(qargv, 256 * 1024, 3000, &qres) == 0 &&
        fmon_child_ok(&qres) && qres.out != NULL)
        parse_nvml_q(qres.out, nv, nv_count);

    fmon_child_result_free(&qres);

    const char *dargv[] = { nvsmi, "dmon", "-s", "t", "-c", "1", NULL };
    FmonChildResult dres;

    if (fmon_child_run(dargv, 64 * 1024, 3000, &dres) == 0 &&
        fmon_child_ok(&dres) && dres.out != NULL)
        parse_nvml_pcie(dres.out, nv, nv_count);

    fmon_child_result_free(&dres);
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
            sample_nvml(tmp, n);
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
