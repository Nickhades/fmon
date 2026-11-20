/*
 * disk_meta.h - 磁盘元数据内核 provider
 *
 * 优先：
 *   - DIOCGMEDIASIZE / DIOCGIDENT（所有 CAM 磁盘）
 *   - NVME_GET_CONTROLLER_DATA（NVMe 型号/序列号）
 *   - CAM ATA IDENTIFY（SATA 型号/序列号/转速）
 *   - CAM SCSI INQUIRY（SAS/SCSI 型号）
 *   - kern.cam.<driver>.<unit>.rotating（SSD/HDD）
 *
 * 失败时由 collector_io 回退 geom/diskinfo/base 工具；不猜测。
 */

#ifndef FMON_DISK_META_H
#define FMON_DISK_META_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char     model[64];
    char     serial[32];
    uint64_t size_bytes;
    uint32_t kind;          /* FMON_DISK_*（含接口位） */
    uint32_t rotation_rpm;
    bool     valid;
} FmonDiskMeta;

bool fmon_disk_meta_probe(const char *name, FmonDiskMeta *out);

#endif /* FMON_DISK_META_H */
