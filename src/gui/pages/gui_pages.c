/*
 * gui_pages.c - 各指标页的数值刷新
 *
 * 所有函数只读 gui_state_snapshot() 的快照，不碰共享内存映射。
 */

#include "gui_pages.h"

#include "gui_state.h"
#include "gui_page_state.h"
#include "gui_chart.h"
#include "alerts.h"
#include "history/long_history.h"
#include "palette.h"
#include "gui_util.h"
#include "zfs_stats.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>

static const FmonShmData *snap(void)
{
    return gui_state_snapshot();
}

static void set_val(MetricPage *p, int i, const char *text)
{
    if (i >= 0 && i < 12 && p->vals[i] != NULL)
        gtk_label_set_text(GTK_LABEL(p->vals[i]), text);
}

void gui_upd_zpool(MetricPage *p)
{
    char b[96];

    if ((snap()->probe_valid & FMON_PROBE_POOLS) == 0) {
        set_val(p, 0, "zpool 不可用");
        for (int i = 1; i < 5; i++)
            set_val(p, i, "—");
        return;
    }

    if (snap()->pool_count == 0 || !snap()->pools[0].available) {
        set_val(p, 0, "无可用池");
        for (int i = 1; i < 5; i++)
            set_val(p, i, "—");
        return;
    }

    const FmonPoolInfo *z = &snap()->pools[0];

    set_val(p, 0, z->name);
    set_val(p, 1, z->health);

    g_snprintf(b, sizeof(b), "%.2f / %.2f GB",
               (double)z->alloc_bytes / (1024.0 * 1024.0 * 1024.0),
               (double)z->total_bytes / (1024.0 * 1024.0 * 1024.0));
    set_val(p, 2, b);

    g_snprintf(b, sizeof(b), "%.1f %%", z->util_pct);
    set_val(p, 3, b);

    char *freeb = fmon_gui_format_bytes(z->free_bytes);

    set_val(p, 4, freeb);
    g_free(freeb);
}

void gui_upd_arc(MetricPage *p)
{
    const ZfsStats *zs = zfs_stats_get();
    char b[96];

    guint64 size = zs ? zs->size : snap()->zfs_arc_size;
    guint64 max  = zs ? zs->c_max : snap()->zfs_arc_c_max;

    char *s = fmon_gui_format_bytes(size);
    char *m = fmon_gui_format_bytes(max);

    set_val(p, 0, s);
    set_val(p, 1, m);
    g_free(s);
    g_free(m);

    g_snprintf(b, sizeof(b), "%.1f %%",
               (max > 0) ? 100.0 * (double)size / (double)max : 0.0);
    set_val(p, 2, b);

    if (zs == NULL) {
        for (int i = 3; i < 10; i++)
            set_val(p, i, "—");
        return;
    }

    char *target = fmon_gui_format_bytes(zs->c);
    char *data   = fmon_gui_format_bytes(zs->data_size);
    char *meta   = fmon_gui_format_bytes(zs->metadata_size);
    char *dnode  = fmon_gui_format_bytes(zs->dnode_size);
    char *dbuf   = fmon_gui_format_bytes(zs->dbuf_size);
    char *avail  = fmon_gui_format_bytes(snap()->mem_avail_bytes);

    set_val(p, 3, target);
    set_val(p, 4, data);
    set_val(p, 5, meta);
    set_val(p, 6, dnode);
    set_val(p, 7, dbuf);
    set_val(p, 8, avail);

    g_snprintf(b, sizeof(b), "%llu 次",
               (unsigned long long)zs->mem_throttle);
    set_val(p, 9, b);

    g_free(target);
    g_free(data);
    g_free(meta);
    g_free(dnode);
    g_free(dbuf);
    g_free(avail);
}

void gui_upd_zhit(MetricPage *p)
{
    const ZfsStats *zs = zfs_stats_get();
    char b[96];

    static guint64 prev_mru, prev_mfu;
    static gint64  prev_us;

    double pct = zs ? zfs_stats_hit_pct(zs->hits, zs->misses)
                    : snap()->zfs_arc_pct;

    g_snprintf(b, sizeof(b), "%.2f %%", pct);
    set_val(p, 0, b);

    if (zs == NULL) {
        for (int i = 1; i < 9; i++)
            set_val(p, i, "—");
        return;
    }

    /* 分类型命中率：各自的 hits/(hits+misses)，不是占全部命中的比例 */
    g_snprintf(b, sizeof(b), "%.2f %%",
               zfs_stats_hit_pct(zs->demand_data_hits,
                                 zs->demand_data_misses));
    set_val(p, 1, b);

    g_snprintf(b, sizeof(b), "%.2f %%",
               zfs_stats_hit_pct(zs->demand_meta_hits,
                                 zs->demand_meta_misses));
    set_val(p, 2, b);

    g_snprintf(b, sizeof(b), "%.2f %%",
               zfs_stats_hit_pct(zs->prefetch_data_hits +
                                     zs->prefetch_meta_hits,
                                 zs->prefetch_data_misses +
                                     zs->prefetch_meta_misses));
    set_val(p, 3, b);

    if (zs->l2_hits + zs->l2_misses > 0)
        g_snprintf(b, sizeof(b), "%.2f %%",
                   zfs_stats_hit_pct(zs->l2_hits, zs->l2_misses));
    else
        g_snprintf(b, sizeof(b), "无 L2");
    set_val(p, 6, b);

    /* 幽灵命中速率（次/秒）：被淘汰后很快又被读取 = 容量与工作集不匹配 */
    gint64 now = g_get_monotonic_time();
    double dt  = (prev_us > 0) ? (double)(now - prev_us) / 1e6 : 0.0;

    g_snprintf(b, sizeof(b), "%.1f 次/秒",
               (dt > 0.0) ? zfs_stats_rate(zs->mru_ghost_hits, prev_mru, dt)
                          : 0.0);
    set_val(p, 4, b);

    g_snprintf(b, sizeof(b), "%.1f 次/秒",
               (dt > 0.0) ? zfs_stats_rate(zs->mfu_ghost_hits, prev_mfu, dt)
                          : 0.0);
    set_val(p, 5, b);

    prev_mru = zs->mru_ghost_hits;
    prev_mfu = zs->mfu_ghost_hits;
    prev_us  = now;

    g_snprintf(b, sizeof(b), "%llu 次",
               (unsigned long long)zs->evict_skip);
    set_val(p, 7, b);

    g_snprintf(b, sizeof(b), "%llu 次",
               (unsigned long long)zs->hash_collisions);
    set_val(p, 8, b);
}

void gui_upd_zread(MetricPage *p)
{
    const FmonPoolInfo *z = (snap()->pool_count > 0) ? &snap()->pools[0] : NULL;
    char *rate  = fmon_gui_format_rate(z ? z->read_kbps : 0.0);
    char *total = fmon_gui_format_bytes(z ? z->read_bytes : 0);
    char  b[96];

    set_val(p, 0, rate);
    set_val(p, 1, total);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)(z ? z->read_ops : 0));
    set_val(p, 2, b);

    const ZfsStats *zs = zfs_stats_get();

    if (zs != NULL && zs->l2_size > 0) {
        char *l2 = fmon_gui_format_bytes((uint64_t)zs->l2_read_bps);
        g_snprintf(b, sizeof(b), "%s/s", l2);
        g_free(l2);
        set_val(p, 3, b);
    } else {
        set_val(p, 3, "—（无 L2）");
    }

    g_free(rate);
    g_free(total);
}

void gui_upd_zwrite(MetricPage *p)
{
    const FmonPoolInfo *z = (snap()->pool_count > 0) ? &snap()->pools[0] : NULL;
    char *rate  = fmon_gui_format_rate(z ? z->write_kbps : 0.0);
    char *total = fmon_gui_format_bytes(z ? z->write_bytes : 0);
    char  b[96];

    set_val(p, 0, rate);
    set_val(p, 1, total);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)(z ? z->write_ops : 0));
    set_val(p, 2, b);

    const ZfsStats *zs = zfs_stats_get();

    if (zs != NULL && zs->l2_size > 0) {
        char *l2 = fmon_gui_format_bytes((uint64_t)zs->l2_write_bps);
        g_snprintf(b, sizeof(b), "%s/s", l2);
        g_free(l2);
        set_val(p, 3, b);
    } else {
        set_val(p, 3, "—（无 L2）");
    }

    g_free(rate);
    g_free(total);
}

void gui_upd_zprune(MetricPage *p)
{
    const ZfsStats *zs = zfs_stats_get();
    char b[96];

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)(zs ? zs->arc_prune : snap()->zfs_arc_prune));
    set_val(p, 0, b);

    if (zs == NULL) {
        for (int i = 1; i < 6; i++)
            set_val(p, i, "—");
        return;
    }

    g_snprintf(b, sizeof(b), "%.1f 次/秒", zs->prune_per_s);
    set_val(p, 1, b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)zs->evict_skip);
    set_val(p, 2, b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)zs->evict_not_enough);
    set_val(p, 3, b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)zs->mem_throttle);
    set_val(p, 4, b);

    char *arc = fmon_gui_format_bytes(zs->size);

    set_val(p, 5, arc);
    g_free(arc);
}

void gui_upd_temp(MetricPage *p)
{
    char b[96];

    if (snap()->cpu_temp_c > 0.0) {
        g_snprintf(b, sizeof(b), "%.1f °C", snap()->cpu_temp_c);
        set_val(p, 0, b);
    } else {
        set_val(p, 0, "不可用");
    }

    g_snprintf(b, sizeof(b), "%u", snap()->cpu_count);
    set_val(p, 1, b);

    set_val(p, 2, (snap()->hostname[0] != '\0') ? snap()->hostname : "unknown");
    set_val(p, 3, (snap()->sys_version[0] != '\0') ? snap()->sys_version : "FreeBSD");
}

void gui_upd_batt(MetricPage *p)
{
    char b[96];

    if ((snap()->probe_valid & FMON_PROBE_BATTERY) == 0) {
        set_val(p, 0, "无电池或探测不可用");
        for (int i = 1; i < 4; i++)
            set_val(p, i, "—");
        return;
    }

    if (!snap()->battery_present) {
        set_val(p, 0, "无电池（台式机）");
        for (int i = 1; i < 4; i++)
            set_val(p, i, "—");
        return;
    }

    g_snprintf(b, sizeof(b), "%.0f %%", snap()->battery_pct);
    set_val(p, 0, b);

    g_snprintf(b, sizeof(b), "%.1f W", snap()->battery_discharge_w);
    set_val(p, 1, b);

    set_val(p, 2, snap()->acline ? "已接通外部电源" : "使用电池");

    if ((snap()->battery_flags & FMON_BATT_CHARGING) != 0)
        set_val(p, 3, "充电中");
    else if ((snap()->battery_flags & FMON_BATT_DISCHARGING) != 0)
        set_val(p, 3, "放电中");
    else
        set_val(p, 3, "待机");
}

/* ===========================================================================
 * ZFS 合并页：多条曲线同框 + 分组详情
 * ========================================================================= */

static CurveData s_curves[METRIC_PAGE_COUNT];

static const FmonColorSlot g_metric_slots[METRIC_PAGE_COUNT] = {
    FMON_COLOR_CPU,       FMON_COLOR_MEM,       FMON_COLOR_SWAP,
    FMON_COLOR_NET_RX,    FMON_COLOR_GPU,       FMON_COLOR_DISK_READ,
    FMON_COLOR_POOL,      FMON_COLOR_ARC,       FMON_COLOR_HIT,
    FMON_COLOR_DISK_READ, FMON_COLOR_DISK_WRITE, FMON_COLOR_ARC,
    FMON_COLOR_TEMP,      FMON_COLOR_BATTERY,
};

void gui_metric_curves_sync(const FmonShmData *snap)
{
    /* 长历史模式使用 provider 的 count/index，不能被实时快照覆盖。 */
    if (gui_long_history_active())
        return;

    for (int i = 0; i < METRIC_PAGE_COUNT; i++) {
        s_curves[i].count        = snap->history_count;
        s_curves[i].index        = snap->history_index;
        s_curves[i].end_time_us  = snap->last_update_us;
        s_curves[i].time_us      = snap->history_time_us;
        s_curves[i].interval_us  =
            (snap->history_interval_us > 0)
                ? snap->history_interval_us
                : (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
    }
}

CurveData *gui_metric_curve(int idx)
{
    if (idx < 0 || idx >= METRIC_PAGE_COUNT)
        return NULL;

    return &s_curves[idx];
}

void gui_metric_curves_bind(const FmonShmData *snap)
{
    static const struct {
        double      max;
        const char *unit;
    } defs[METRIC_PAGE_COUNT] = {
        { 100.0, "%" },     /* 0 CPU */
        { 100.0, "%" },     /* 1 内存 */
        { 100.0, "%" },     /* 2 Swap */
        { 0.0,   "KB/s" },  /* 3 网络 RX */
        { 100.0, "%" },     /* 4 GPU */
        { 0.0,   "KB/s" },  /* 5 磁盘读 */
        { 100.0, "%" },     /* 6 zpool 空间 */
        { 100.0, "%" },     /* 7 ARC */
        { 100.0, "%" },     /* 8 命中率 */
        { 0.0,   "KB/s" },  /* 9 zpool 读 */
        { 0.0,   "KB/s" },  /* 10 zpool 写 */
        { 0.0,   "次" },    /* 11 ARC Prune */
        { 100.0, "°C" },    /* 12 温度 */
        { 100.0, "%" },     /* 13 电池 */
    };

    const double *hists[METRIC_PAGE_COUNT] = {
        snap->cpu_history,        snap->mem_history,       snap->swap_history,
        snap->net_rx_history,     snap->gpu_history[0],    snap->disk_read_history,
        snap->zpool_util_history, snap->zfs_arc_history,   snap->zfs_hit_history,
        snap->zfs_io_read_history, snap->zfs_io_write_history,
        snap->zfs_arc_prune_history, snap->temp_history,   snap->battery_history,
    };

    static const int alert_metrics[METRIC_PAGE_COUNT] = {
        0, 1, 2, -1, -1, -1, 6, -1, -1, -1, -1, -1, 12, 13
    };

    for (int i = 0; i < METRIC_PAGE_COUNT; i++) {
        s_curves[i].history        = hists[i];
        s_curves[i].history_min    = NULL;
        s_curves[i].history_max    = NULL;
        s_curves[i].view_start_us  = 0;
        s_curves[i].view_end_us    = 0;
        s_curves[i].fixed_range    = FALSE;
        s_curves[i].frozen         = FALSE;
        s_curves[i].alert_settings = alerts_current();
        s_curves[i].alert_metric   = alert_metrics[i];

        /* ARC / 命中率：下限跟随数据，避免曲线贴顶看不出变化 */
        s_curves[i].min_val =
            (i == 7 || i == 8 || i == 13) ? -1.0 : 0.0;
        double color[4];

        fmon_palette_get(g_metric_slots[i], color);
        s_curves[i].r           = color[0];
        s_curves[i].g           = color[1];
        s_curves[i].b           = color[2];
        s_curves[i].max_val     = defs[i].max;
        s_curves[i].unit        = defs[i].unit;
        s_curves[i].time_us     = snap->history_time_us;
        s_curves[i].interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
    }

    gui_metric_curves_apply_theme();
}

void gui_metric_curves_apply_theme(void)
{
    for (int i = 0; i < METRIC_PAGE_COUNT; i++) {
        double color[4];

        fmon_palette_get(g_metric_slots[i], color);
        s_curves[i].r = color[0];
        s_curves[i].g = color[1];
        s_curves[i].b = color[2];
    }

    double c[4];

    fmon_palette_get(FMON_COLOR_NET_RX, c);
    g_net_rx_cd.r = c[0]; g_net_rx_cd.g = c[1]; g_net_rx_cd.b = c[2];
    fmon_palette_get(FMON_COLOR_NET_TX, c);
    g_net_tx_cd.r = c[0]; g_net_tx_cd.g = c[1]; g_net_tx_cd.b = c[2];
    fmon_palette_get(FMON_COLOR_DISK_READ, c);
    g_disk_rd_cd.r = c[0]; g_disk_rd_cd.g = c[1]; g_disk_rd_cd.b = c[2];
    fmon_palette_get(FMON_COLOR_DISK_WRITE, c);
    g_disk_wr_cd.r = c[0]; g_disk_wr_cd.g = c[1]; g_disk_wr_cd.b = c[2];
    fmon_palette_get(FMON_COLOR_POOL, c);
    g_zpool_cd.r = c[0]; g_zpool_cd.g = c[1]; g_zpool_cd.b = c[2];
}
