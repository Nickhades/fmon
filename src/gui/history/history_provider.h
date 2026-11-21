/*
 * history_provider.h - 主界面长历史数据源（1m/5m/1h 层）
 *
 * 只读 GUI 侧缓存；不写 SHM，不拥有采样线程。
 */
#ifndef FMON_HISTORY_PROVIDER_H
#define FMON_HISTORY_PROVIDER_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#define FMON_HISTORY_MAX_POINTS 1200

typedef enum {
    FMON_HIST_CPU = 0,
    FMON_HIST_MEM,
    FMON_HIST_SWAP,
    FMON_HIST_NET_RX,
    FMON_HIST_NET_TX,
    FMON_HIST_DISK_RD,
    FMON_HIST_DISK_WR,
    FMON_HIST_TEMP,
    FMON_HIST_DISK_TEMP,
    FMON_HIST_COUNT
} FmonHistoryMetric;

typedef struct {
    const double *values;
    const double *min;
    const double *max;
    bool          available;
} FmonHistorySeries;

/* 加载最近 range_secs 的 1 分钟金字塔；失败返回 FALSE。 */
bool fmon_history_load(gint64 range_secs);
void fmon_history_unload(void);
bool fmon_history_loaded(void);

const FmonHistorySeries *fmon_history_series(FmonHistoryMetric metric);
const uint64_t          *fmon_history_time_us(void);
uint32_t                 fmon_history_count(void);
uint32_t                 fmon_history_interval_s(void);

typedef enum {
    FMON_ZFS_HIST_ZPOOL_UTIL = 0,
    FMON_ZFS_HIST_ARC_PCT,
    FMON_ZFS_HIST_L2_SIZE,
    FMON_ZFS_HIST_L2_HIT,
    FMON_ZFS_HIST_READ_KBPS,
    FMON_ZFS_HIST_WRITE_KBPS,
    FMON_ZFS_HIST_COUNT
} FmonZfsHistoryMetric;

bool fmon_zfs_history_load(gint64 range_secs);
void fmon_zfs_history_unload(void);
const FmonHistorySeries *fmon_zfs_history_series(FmonZfsHistoryMetric metric);
const uint64_t *fmon_zfs_history_time_us(void);
uint32_t fmon_zfs_history_count(void);

typedef enum {
    FMON_GPU_HIST_USAGE = 0,
    FMON_GPU_HIST_TEMP,
    FMON_GPU_HIST_MEM,
    FMON_GPU_HIST_COUNT
} FmonGpuHistoryMetric;

bool fmon_gpu_history_load(gint64 range_secs);
void fmon_gpu_history_unload(void);
const FmonHistorySeries *fmon_gpu_history_series(int slot,
                                                 FmonGpuHistoryMetric metric);
const uint64_t *fmon_gpu_history_time_us(int slot);
uint32_t fmon_gpu_history_count(int slot);

#endif /* FMON_HISTORY_PROVIDER_H */
