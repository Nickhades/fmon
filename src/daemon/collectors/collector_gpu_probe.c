/*
 * collector_gpu_probe.c - GPU PCI 枚举与型号表
 */

#include "collector_gpu_probe.h"
#include "fmon_daemon.h"
#include "collector_gpu_internal.h"

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

#include "fmon_child.h"
#include "fmon_sysctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FMON_DEFAULT_PATH FMON_LOCALBASE "/bin:" FMON_LOCALBASE "/sbin:" \
                          "/usr/bin:/bin:/usr/sbin:/sbin:" \
                          "/usr/local/nvidia/bin:/opt/bin"



bool gpu_resolve_binary(const char *name, char *out, size_t out_len)
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

bool gpu_have_binary(const char *name)
{
    char buf[1024];

    return gpu_resolve_binary(name, buf, sizeof(buf));
}

const GpuPciName *gpu_lookup_pci_name(unsigned vendor, unsigned device)
{
    for (size_t i = 0; i < FMON_ARRAY_LEN(k_gpu_names); i++) {
        if (k_gpu_names[i].vendor == vendor && k_gpu_names[i].device == device)
            return &k_gpu_names[i];
    }
    return NULL;
}

bool gpu_add_desc(const char *name, const char *vendor, const char *pci_id,
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
/* 从 pciconf -lv 里解析所有显示类设备（class 0x030000 VGA / 0x030200 3D）。
 * 好处：即使驱动没加载也能拿到型号名（来自 PCI 厂商数据库），
 * 而 DRM 节点只有在驱动加载后才存在。 */
int gpu_scan_pciconf(PciGpu *out, int max)
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

        /* pciconf 输出中，每个设备头都是顶格行；其后的
         * vendor/device 是缩进行。必须在新设备头处清空 cur，
         * 否则 NVMe/网卡等后续设备的型号会覆盖上一张显卡。
         * 这正是“NVIDIA NVMe SSD Controller”出现在 GPU 页的原因。 */
        if (line[0] != ' ' && line[0] != '\t') {
            cur = NULL;

            if (strncmp(line, "vgapci", 6) != 0 ||
                strchr(line, '@') == NULL)
                continue;

            bool display = (strstr(line, "class=0x030000") != NULL ||
                            strstr(line, "class=0x030200") != NULL);
            if (!display || n >= max)
                continue;

            PciGpu *d = &out[n];
            memset(d, 0, sizeof(*d));

            /* 槽位：vgapci0@pci0:1:0:0: → "pci0:1:0:0"
             * 取 @ 之后到空白之前，再去掉结尾的冒号。 */
            const char *at  = strchr(line, '@') + 1;
            const char *end = at;

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

const char *gpu_short_vendor(unsigned vid)
{
    switch (vid) {
    case 0x10de: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    default:     return "Unknown";
    }
}
