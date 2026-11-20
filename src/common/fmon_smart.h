/*
 * fmon_smart.h - SMART / 温度原始数据结构解析（纯 C，无外部工具）
 *
 * 首选输入是内核 ioctl/CAM 返回的二进制页；文本解析仅用于无内核接口时的
 * nvmecontrol（base）/ smartctl（可选）回退。
 */

#ifndef FMON_SMART_H
#define FMON_SMART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool   valid;
    double temp_c;
    int    spare_pct;   /* -1 = 该结构没有此字段 */
    int    wear_pct;    /* -1 = 该结构没有此字段 */
} FmonSmartInfo;

/* 数据来源，写入 FmonDiskInfo.smart_source，便于界面/排错。 */
#define FMON_SMART_SRC_NONE        0u
#define FMON_SMART_SRC_NVME_IOCTL  1u
#define FMON_SMART_SRC_ATA_CAM     2u
#define FMON_SMART_SRC_SCSI_CAM    3u
#define FMON_SMART_SRC_NVMECONTROL 4u
#define FMON_SMART_SRC_SMARTCTL    5u

/* NVMe SMART/Health Information log page（512 字节，小端）。 */
bool fmon_smart_parse_nvme_health(const uint8_t *buf, size_t len,
                                  FmonSmartInfo *out);

/* ATA SMART READ DATA（512 字节，属性表在 offset 2，30 项 ×12）。 */
bool fmon_smart_parse_ata_smart(const uint8_t *buf, size_t len,
                                FmonSmartInfo *out);

/* SCSI LOG SENSE page 0x0D（Temperature log page）。 */
bool fmon_smart_parse_scsi_temperature(const uint8_t *buf, size_t len,
                                       FmonSmartInfo *out);

/* 基础/可选工具回退路径的文本解析（仅在无内核接口时使用）。 */
bool fmon_smart_parse_nvmecontrol(const char *text, FmonSmartInfo *out);
bool fmon_smart_parse_smartctl(const char *text, FmonSmartInfo *out);

#ifdef __cplusplus
}
#endif

#endif /* FMON_SMART_H */
