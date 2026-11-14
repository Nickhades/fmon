/*
 * zpool_health.h - ZFS 池健康（碎片率、错误计数、scrub 结果）
 *
 * 每 15 秒异步跑一次 `zpool status -v` 与 `zpool list -Hp -o name,frag`，
 * 解析池名、状态、config 表里的 READ/WRITE/CKSUM 错误、scan 行（scrub/ resilver）。
 * 不占用共享内存 ABI，普通用户即可读取。
 */

#ifndef ZPOOL_HEALTH_H
#define ZPOOL_HEALTH_H

#include <glib.h>

typedef struct {
    gboolean valid;
    char     pool[32];
    char     state[16];
    int      frag_pct;       /* -1 = 不可用 */
    guint64  read_err;
    guint64  write_err;
    guint64  cksum_err;
    gboolean has_scan;
    char     scan[192];      /* "scrub repaired 0B in ... with 0 errors ..." */

    /* 来自 zpool iostat -H -l -q 1 1 */
    gboolean has_iostat;
    double   total_wait_r_ms;
    double   total_wait_w_ms;
    double   disk_wait_r_ms;
    double   disk_wait_w_ms;
    double   q_pend;
    double   q_activ;

    guint64  slow_io;      /* zpool status -s 的 SLOW 列合计 */

    /* 延迟直方图（zpool iostat -w 的 total_wait 读+写） */
    int      lat_n;
    struct {
        char  label[12];
        double count;
    } lat[12];

    gboolean has_slog;     /* config 里是否有 logs（SLOG）段 */
    char     slog[96];     /* 第一个 log 设备名 */

    /* 数据集级 kstat（逻辑写入 / ZIL） */
    guint64 ds_written;    /* 所有数据集 nwritten 之和 */
    guint64 zil_commit;    /* zil_commit_count 之和 */
    guint64 zil_stall;     /* zil_commit_stall_count 之和 */
    guint64 zil_itx;       /* zil_itx_count 之和 */

    /* 脏数据节流（kstat.zfs.misc.dmu_tx.*） */
    guint64 dmu_dirty_delay;
    guint64 dmu_dirty_throttle;
    guint64 dmu_wrlog_delay;

    /* 逻辑已用（zfs get used <pool>） */
    guint64 used_logical;

    /* scrub 进度（in progress 时） */
    char scan_pct[16];
    char scan_eta[24];

    /* 写请求大小分布（zpool iostat -r 聚合） */
    double req_small;      /* ≤4K */
    double req_mid;        /* 8K–64K */
    double req_large;      /* ≥128K */

    /* TXG（kstat.zfs.<pool>.txgs，最近一个已完成的事务组） */
    gboolean has_txg;
    guint64  txg;
    double   stime_ms;
    double   txg_avg_ms;    /* 最近若干次已完成 TXG 的平均 stime */
    double   txg_max_ms;
    double   txg_qtime_ms;  /* 最近一次已完成 TXG 的队列等待 */
    double   txg_wtime_ms;  /* 写延迟 */
    double   txg_otime_ms;  /* 总打开时间 */
    int      txg_samples;
    gboolean txg_spike;     /* 最近一次 stime 明显高于均值 */

    /* vdev 级（zpool iostat -Hpv -p） */
    int      vdev_count;
    struct {
        char   name[32];
        double rops, wops;
        double rbps, wbps;
        double tw_r_ms, tw_w_ms;   /* total_wait 读/写 */
        double dw_r_ms, dw_w_ms;   /* disk_wait 读/写 */
        double sq_r_ms, sq_w_ms;   /* syncq_wait 读/写 */
    } vdev[8];
} ZpoolHealth;

void zpool_health_update(void);
const ZpoolHealth *zpool_health_get(void);

/* 纯函数，便于单测 */
gboolean zpool_health_parse_status(const char *text, ZpoolHealth *out);
gboolean zpool_health_parse_frag(const char *text, ZpoolHealth *out);

/* 纯函数：解析 zpool iostat -H -l -q 1 1（单行池汇总） */
gboolean zpool_health_parse_iostat(const char *text, ZpoolHealth *out);

/* 纯函数：解析 zpool iostat -Hpv -p 1 1（第一行是池，其余是 vdev） */
int zpool_health_parse_vdevs(const char *text, ZpoolHealth *out);

/* 纯函数：解析 `sysctl -n kstat.zfs.<pool>.txgs` 的文本表，
 * 取最后一个 stime>0 的已完成 TXG。 */
gboolean zpool_health_parse_txg(const char *text, ZpoolHealth *out);

/* 纯函数：解析 `zpool iostat -w` 的延迟直方图桶 */
gboolean zpool_health_parse_latency(const char *text, ZpoolHealth *out);

/* 纯函数：解析“每行一个数字”的 sysctl 输出，按行求和 */
guint64 zpool_health_sum_lines(const char *text);

/* 纯函数：解析 `zpool iostat -r` 的写请求大小分布 */
gboolean zpool_health_parse_reqsize(const char *text, ZpoolHealth *out);

#endif /* ZPOOL_HEALTH_H */
