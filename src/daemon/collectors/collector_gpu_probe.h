/*
 * collector_gpu_probe.h - GPU PCI 枚举
 */

#ifndef FMON_COLLECTOR_GPU_PROBE_H
#define FMON_COLLECTOR_GPU_PROBE_H

#include "collector_gpu_internal.h"

typedef struct {
    uint16_t    vendor;
    uint16_t    device;
    const char *name;
    uint32_t    kind;
} GpuPciName;

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



bool gpu_resolve_binary(const char *name, char *out, size_t out_len);
bool gpu_have_binary(const char *name);
bool gpu_add_desc(const char *name, const char *vendor, const char *pci_id,
                  unsigned pci_device, uint32_t kind, uint32_t sensor,
                  uint32_t driver_loaded);
const char *gpu_short_vendor(unsigned vid);
int gpu_scan_pciconf(PciGpu *out, int max);
const GpuPciName *gpu_lookup_pci_name(unsigned vendor, unsigned device);

#endif
