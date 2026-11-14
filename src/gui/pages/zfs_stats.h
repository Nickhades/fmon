/*
 * zfs_stats.h - ZFS ARC 细节（GUI 直接读 kstat sysctl）
 *
 * 共享内存里只有 ARC 大小/上限/命中/幽灵命中等少量字段；要画出
 * MFU/MRU 构成、命中分类、回收压力、L2 流量，需要读
 * `kstat.zfs.misc.arcstats.*`。这些 sysctl 普通用户可读，1 Hz 采样
 * 开销很小，所以放在 GUI 侧，避免为纯展示数据升 ABI。
 */

#ifndef ZFS_STATS_H
#define ZFS_STATS_H

#include <glib.h>

typedef struct {
    /* 容量 */
    guint64 size;          /* arcstats.size */
    guint64 c_max;         /* arcstats.c_max */
    guint64 mru_size;      /* arcstats.mru_size */
    guint64 mfu_size;      /* arcstats.mfu_size */
    guint64 meta_used;     /* arcstats.arc_meta_used */
    guint64 l2_size;       /* arcstats.l2_size */

    /* ARC 构成细分（字节） */
    guint64 dnode_size;    /* dnode 元数据 */
    guint64 dbuf_size;     /* dbuf 元数据 */
    guint64 metadata_size; /* 元数据总量 */
    guint64 data_size;     /* 数据缓冲 */
    guint64 hdr_size;      /* 头部开销 */

    /* 目标值与水位 */
    guint64 c;             /* 当前目标大小 */
    guint64 c_min;

    /* 命中（累计） */
    guint64 hits, misses;
    guint64 demand_data_hits, demand_meta_hits;
    guint64 prefetch_data_hits, prefetch_meta_hits;
    guint64 demand_data_misses, demand_meta_misses;
    guint64 prefetch_data_misses, prefetch_meta_misses;
    guint64 mru_ghost_hits, mfu_ghost_hits;
    guint64 l2_hits, l2_misses;

    /* 回收压力（累计）与速率 */
    guint64 arc_prune, evict_skip, evict_not_enough, mem_throttle;
    guint64 hash_collisions;
    double  prune_per_s;

    /* 回收路径细节 */
    guint64 access_skip, mutex_miss, deleted;

    /* 内存压力 */
    guint64 no_grow, need_free, tempreserve, sys_free;
    guint64 mem_free, mem_avail, mem_all, mem_direct, mem_indirect;

    /* 可回收池与幽灵大小 */
    guint64 mru_evictable_data, mru_evictable_meta;
    guint64 mfu_evictable_data, mfu_evictable_meta;
    guint64 anon_evictable_data, anon_evictable_meta, anon_size;
    guint64 mru_ghost_size, mfu_ghost_size;

    /* 哈希表 */
    guint64 hash_elements, hash_elements_max, hash_chains;

    /* L2 流量（累计）与速率（字节/秒） */
    guint64 l2_read_bytes, l2_write_bytes;
    double  l2_read_bps, l2_write_bps;

    /* L2ARC 明细 */
    guint64 l2_ndev;            /* 设备数 */
    guint64 l2_asize;           /* 实际分配（含压缩） */
    guint64 l2_hdr_size;        /* 头部开销 */
    guint64 l2_writes_sent, l2_writes_done, l2_writes_error;
    guint64 l2_evict_reading, l2_evict_l1cached, l2_evict_lock_retry;
    guint64 l2_free_on_write;
    guint64 l2_io_error, l2_cksum_bad, l2_abort_lowmem;
    guint64 l2_feeds, l2_rw_clash;
    guint64 l2_rebuild_success, l2_rebuild_size, l2_rebuild_io_errors;
    guint64 l2_mfu_asize, l2_mru_asize, l2_prefetch_asize;

    /* ARC 压缩收益（top 的 Compressed/Uncompressed 行） */
    guint64 compressed_size, uncompressed_size;
} ZfsStats;

/* 每秒调用一次；失败时下一次 zfs_stats_get() 返回 NULL。 */
void zfs_stats_update(void);

/* 采集线程用：collect 只读 kstat，publish 在主线程发布。 */
void zfs_stats_collect(ZfsStats *out);
void zfs_stats_publish(const ZfsStats *in);

/* 最近一次采样；无 ZFS 或读取失败返回 NULL。 */
const ZfsStats *zfs_stats_get(void);

/* 纯函数：命中率（0–100），无访问时返回 0。 */
double zfs_stats_hit_pct(guint64 hits, guint64 misses);

/* 纯函数：累计计数器的每秒速率，计数器回绕/重置时返回 0。 */
double zfs_stats_rate(guint64 cur, guint64 prev, double dt);

/* 回收风暴判据结果 */
typedef enum {
    ZFS_RECLAIM_IDLE = 0,   /* 无回收活动 */
    ZFS_RECLAIM_NORMAL,     /* 有回收但无内存压力 */
    ZFS_RECLAIM_PRESSURE,   /* 回收突增 + 节流/no_grow = 内存压力强制回收 */
} ZfsReclaimState;

/* 纯函数：按 prune/evict_skip 速率与节流增量判断回收状态 */
ZfsReclaimState zfs_stats_reclaim_state(double prune_s, double skip_s,
                                        guint64 throttle_delta,
                                        guint64 no_grow);

#endif /* ZFS_STATS_H */
