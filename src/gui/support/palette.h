/*
 * palette.h - 语义化指标配色
 *
 * 终端 ANSI 风格：CPU 绿、内存/网络接收青、Swap/网络发送洋红、
 * 磁盘读黄、磁盘写橙、温度红、GPU 黄。亮色用于暗色主题，
 * 浅色主题使用同色相暗档。
 */
#ifndef FMON_PALETTE_H
#define FMON_PALETTE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMON_COLOR_CPU = 0,
    FMON_COLOR_MEM,
    FMON_COLOR_SWAP,
    FMON_COLOR_NET_RX,
    FMON_COLOR_NET_TX,
    FMON_COLOR_DISK_READ,
    FMON_COLOR_DISK_WRITE,
    FMON_COLOR_TEMP,
    FMON_COLOR_GPU,
    FMON_COLOR_POOL,
    FMON_COLOR_ARC,
    FMON_COLOR_HIT,
    FMON_COLOR_BATTERY,
    FMON_COLOR_COUNT
} FmonColorSlot;

void fmon_palette_get(FmonColorSlot slot, double rgba[4]);

#ifdef __cplusplus
}
#endif

#endif /* FMON_PALETTE_H */
