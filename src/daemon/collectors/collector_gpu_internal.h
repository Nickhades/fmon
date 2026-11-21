/*
 * collector_gpu_internal.h - GPU 描述与 CSV 辅助
 */

#ifndef FMON_COLLECTOR_GPU_INTERNAL_H
#define FMON_COLLECTOR_GPU_INTERNAL_H

#include "fmon_shm.h"

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

extern GpuDesc g_descs[FMON_MAX_GPUS];
extern int g_desc_count;
bool gpu_resolve_binary(const char *name, char *out, size_t out_len);
char *gpu_csv_next(char **save, char *buf, size_t len);

#endif
