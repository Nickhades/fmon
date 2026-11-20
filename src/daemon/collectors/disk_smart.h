/*
 * disk_smart.h - 磁盘温度/SMART 内核 provider 链
 *
 * provider 顺序：
 *   1. 内核：NVMe ioctl / CAM ATAIO / CAM SCSI LOG SENSE
 *   2. base 工具：nvmecontrol（仅 NVMe）
 *   3. 可选工具：smartctl（smartmontools，存在才用）
 * 设备名来自 kern.disks，控制器由 NVME_GET_NSID 反查，不写死 ndaN→nvmeN。
 */

#ifndef FMON_DISK_SMART_H
#define FMON_DISK_SMART_H

#include <stdbool.h>
#include <stdint.h>

#include "fmon_smart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 ndaN 反查控制器路径；不假设 ndaN → nvmeN。
 * 供 SMART 与磁盘元数据共用。 */
bool fmon_nvme_controller_path(const char *name, char *out, size_t out_len);

/* 30 秒缓存，调用频率与 sample_disk 一致（1 Hz）。
 * 返回时 source 为 FMON_SMART_SRC_*；无 provider 时为 NONE。 */
void fmon_disk_smart_probe(const char *disk_name, int64_t now_ms,
                           FmonSmartInfo *out, uint32_t *source);

#ifdef __cplusplus
}
#endif

#endif /* FMON_DISK_SMART_H */
