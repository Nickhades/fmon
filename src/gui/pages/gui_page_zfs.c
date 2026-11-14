/*
 * gui_page_zfs.c - ZFS 池页右上摘要与详情卡片
 */

#include "gui_page_zfs.h"

#include "gui_state.h"
#include "gui_util.h"
#include "zpool_health.h"
#include "zfs_stats.h"
#include "disk_queue.h"
#include "gui_widgets.h"

#include "combo_chart.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

static GtkWidget *s_pool     = NULL;
static GtkWidget *s_cap      = NULL;
static GtkWidget *s_pct      = NULL;
static GtkWidget *s_lat_r    = NULL;
static GtkWidget *s_lat_w    = NULL;
static GtkWidget *s_queue    = NULL;
static GtkWidget *s_err      = NULL;
static GtkWidget *s_slow     = NULL;
static GtkWidget *s_frag     = NULL;
static GtkWidget *s_frag_h   = NULL;
static GtkWidget *s_arc_h    = NULL;
static GtkWidget *s_txg      = NULL;
static GtkWidget *s_scan     = NULL;
static GtkWidget *s_cap_log  = NULL;
static GtkWidget *s_delta    = NULL;
static GtkWidget *s_dirty    = NULL;
static GtkWidget *s_slog     = NULL;
static GtkWidget *s_vdev_box = NULL;

/* 错误趋势：环形缓冲，每秒记录“新增错误数” */
static double    s_err_hist[FMON_HISTORY_POINTS];
static CurveData s_err_cd = {
    .history = s_err_hist,
    .unit    = "",
    .r = 0.90, .g = 0.25, .b = 0.20,
};
static guint64  s_prev_err;
static gboolean s_have_prev_err;

static GtkWidget *label_x(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_widget_set_halign(l, (xalign >= 1.0f) ? GTK_ALIGN_END : GTK_ALIGN_START);
    return l;
}

static GtkWidget *right_line(void)
{
    GtkWidget *l = label_x("", "hw-model", 1.0f);

    gtk_widget_set_halign(l, GTK_ALIGN_END);
    return l;
}

GtkWidget *gui_zpool_header_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_halign(box, GTK_ALIGN_END);
    gtk_widget_set_valign(box, GTK_ALIGN_START);

    s_pool   = right_line();
    s_cap    = right_line();
    s_pct    = right_line();
    s_frag_h = right_line();
    s_arc_h  = right_line();

    gtk_box_append(GTK_BOX(box), s_pool);
    gtk_box_append(GTK_BOX(box), s_cap);
    gtk_box_append(GTK_BOX(box), s_pct);
    gtk_box_append(GTK_BOX(box), s_frag_h);
    gtk_box_append(GTK_BOX(box), s_arc_h);
    return box;
}

/* 长项独占一行：键 + 跨列的值 */
static void attach_full(GtkWidget *grid, int row, const char *key,
                        GtkWidget *val)
{
    GtkWidget *k = label_x(key, "group-desc", 0.0f);

    gtk_widget_set_halign(k, GTK_ALIGN_START);
    gtk_widget_set_size_request(k, 96, -1);
    gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);

    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_widget_set_valign(val, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), val, 1, row, 4, 1);
}

GtkWidget *gui_zpool_detail_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_margin_top(box, 12);
    gtk_box_append(GTK_BOX(box), label_x(_("池详情"), "detail-title", 0.0f));

    s_lat_r = label_x("—", "detail-value", 0.0f);
    s_lat_w = label_x("—", "detail-value", 0.0f);
    s_queue = label_x("—", "detail-value", 0.0f);
    s_err   = label_x("—", "detail-value", 0.0f);
    s_slow  = label_x("—", "detail-value", 0.0f);
    s_frag  = label_x("—", "detail-value", 0.0f);
    s_txg   = label_x("—", "group-desc", 0.0f);
    s_scan  = label_x("—", "group-desc", 0.0f);
    s_slog  = label_x("—", "group-desc", 0.0f);

    gtk_label_set_wrap(GTK_LABEL(s_scan), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s_scan), 100);

    s_cap_log = label_x("—", "detail-value", 0.0f);
    s_delta   = label_x("—", "detail-value", 0.0f);
    s_dirty   = label_x("—", "group-desc", 0.0f);

    /* 左栏：实时性能；右栏：维护/健康（小字项并排，不再堆在下面） */
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
    GtkWidget *lgrid = gtk_grid_new();
    GtkWidget *rgrid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(lgrid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(lgrid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(rgrid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(rgrid), 10);
    gtk_widget_set_margin_top(cols, 4);

    int r = 0;

    attach_full(lgrid, r++, _("延迟（读）"), s_lat_r);
    attach_full(lgrid, r++, _("延迟（写）"), s_lat_w);
    attach_full(lgrid, r++, _("队列"), s_queue);
    attach_full(lgrid, r++, _("已用（物理 / 逻辑）"), s_cap_log);
    attach_full(lgrid, r++, _("近 1 分钟变化"), s_delta);
    attach_full(lgrid, r++, _("错误"), s_err);
    attach_full(lgrid, r++, _("慢 I/O"), s_slow);

    r = 0;
    attach_full(rgrid, r++, _("TXG"), s_txg);
    attach_full(rgrid, r++, _("脏数据延迟"), s_dirty);
    attach_full(rgrid, r++, _("ZIL / SLOG"), s_slog);

    s_vdev_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    attach_full(rgrid, r++, _("vdev"), s_vdev_box);

    attach_full(rgrid, r++, _("最近 scrub"), s_scan);

    gtk_widget_set_hexpand(rgrid, TRUE);
    gtk_box_append(GTK_BOX(cols), lgrid);
    gtk_box_append(GTK_BOX(cols), rgrid);
    gtk_box_append(GTK_BOX(box), cols);
    return box;
}

CurveData *gui_zpool_err_curve(void)
{
    s_err_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
    return &s_err_cd;
}

/* vdev 行只创建一次，后续只改文本/可见性，避免每秒销毁重建
 * （GTK4 下反复 remove+append 曾在长时间运行时触发崩溃） */
static GtkWidget *s_vdev_lbl[8];
static GtkWidget *s_vdev_empty;

static void rebuild_vdevs(const ZpoolHealth *h)
{
    if (s_vdev_empty == NULL) {
        s_vdev_empty = label_x(_("没有 vdev 数据"), "group-desc", 0.0f);
        gtk_box_append(GTK_BOX(s_vdev_box), s_vdev_empty);
    }

    gtk_widget_set_visible(s_vdev_empty, h->vdev_count == 0);

    for (int i = 0; i < 8; i++) {
        if (i >= h->vdev_count) {
            if (s_vdev_lbl[i] != NULL)
                gtk_widget_set_visible(s_vdev_lbl[i], FALSE);
            continue;
        }

        if (s_vdev_lbl[i] == NULL) {
            s_vdev_lbl[i] = label_x("", "group-desc", 0.0f);
            gtk_box_append(GTK_BOX(s_vdev_box), s_vdev_lbl[i]);
        }

        char *rd = fmon_gui_format_bytes((guint64)h->vdev[i].rbps);
        char *wr = fmon_gui_format_bytes((guint64)h->vdev[i].wbps);
        gchar *txt = g_strdup_printf(
            "%s  读 %s/s · 写 %s/s · IOPS %.0f / %.0f · 延迟 读 %.2f / 写 %.2f ms · syncq %.2f / %.2f ms",
            h->vdev[i].name, rd, wr, h->vdev[i].rops, h->vdev[i].wops,
            h->vdev[i].tw_r_ms, h->vdev[i].tw_w_ms, h->vdev[i].sq_r_ms,
            h->vdev[i].sq_w_ms);

        gtk_label_set_text(GTK_LABEL(s_vdev_lbl[i]), txt);
        gtk_widget_set_visible(s_vdev_lbl[i], TRUE);
        g_free(txt);
        g_free(rd);
        g_free(wr);
    }
}

void gui_zpool_detail_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (s_lat_r == NULL || !gtk_widget_get_mapped(s_lat_r))
        return;   /* 页面不可见时不做格式化/布局 */

    const FmonPoolInfo *z = (snap->pool_count > 0) ? &snap->pools[0] : NULL;

    if (z != NULL) {
        char *alloc = fmon_gui_format_bytes(z->alloc_bytes);
        char *total = fmon_gui_format_bytes(z->total_bytes);
        char *freeb = fmon_gui_format_bytes(z->free_bytes);
        gchar *l1 = g_strdup_printf("%s · %s", z->name, z->health);
        gchar *l2 = g_strdup_printf(_("已用 %s / 总量 %s"), alloc, total);
        gchar *l3 = g_strdup_printf(_("使用率 %.1f %% · 空闲 %s"),
                                    z->util_pct, freeb);

        gtk_label_set_text(GTK_LABEL(s_pool), l1);
        gtk_label_set_text(GTK_LABEL(s_cap), l2);
        gtk_label_set_text(GTK_LABEL(s_pct), l3);
        g_free(l1);
        g_free(l2);
        g_free(l3);
        g_free(alloc);
        g_free(total);
        g_free(freeb);
    }

    zpool_health_update();

    const ZpoolHealth *h = zpool_health_get();

    if (h == NULL) {
        gtk_label_set_text(GTK_LABEL(s_lat_r), _("采集中…"));
        gtk_label_set_text(GTK_LABEL(s_lat_w), _("采集中…"));
        gtk_label_set_text(GTK_LABEL(s_queue), "—");
        gtk_label_set_text(GTK_LABEL(s_err), "—");
        gtk_label_set_text(GTK_LABEL(s_slow), "—");
        gtk_label_set_text(GTK_LABEL(s_frag_h), _("碎片率 —"));
        gtk_label_set_text(GTK_LABEL(s_txg), "—");
        gtk_label_set_text(GTK_LABEL(s_scan), "—");
        return;
    }

    gchar *lat_r, *lat_w;

    if (h->has_iostat) {
        lat_r = g_strdup_printf(_("total %.2f ms · disk %.2f ms"),
                                h->total_wait_r_ms, h->disk_wait_r_ms);
        lat_w = g_strdup_printf(_("total %.2f ms · disk %.2f ms"),
                                h->total_wait_w_ms, h->disk_wait_w_ms);
    } else {
        lat_r = g_strdup("—");
        lat_w = g_strdup("—");
    }

    gtk_label_set_text(GTK_LABEL(s_lat_r), lat_r);
    gtk_label_set_text(GTK_LABEL(s_lat_w), lat_w);
    g_free(lat_r);
    g_free(lat_w);

    gchar *q = g_strdup_printf(_("pend %.0f / activ %.0f"), h->q_pend,
                               h->q_activ);

    gtk_label_set_text(GTK_LABEL(s_queue), q);
    g_free(q);

    /* 物理分配 / 逻辑已用（快照占用的空间体现在两者差值） */
    if (z != NULL) {
        char *alloc = fmon_gui_format_bytes(z->alloc_bytes);
        char *log   = fmon_gui_format_bytes(h->used_logical);
        gchar *cap  = g_strdup_printf("%s / %s", alloc, log);

        gtk_label_set_text(GTK_LABEL(s_cap_log), cap);
        g_free(cap);
        g_free(alloc);
        g_free(log);

        /* 近 1 分钟使用率变化：百分比太小，换算成 MB 才看得出动静 */
        const FmonShmData *ss = gui_state_snapshot();

        if (ss->history_count > 61) {
            guint last = (ss->history_index + FMON_HISTORY_POINTS - 1) %
                         FMON_HISTORY_POINTS;
            guint old  = (last + FMON_HISTORY_POINTS - 60) %
                         FMON_HISTORY_POINTS;
            double d   = ss->zpool_util_history[last] -
                         ss->zpool_util_history[old];
            double mb  = d / 100.0 * (double)z->total_bytes /
                         (1024.0 * 1024.0);

            gchar *dt = g_strdup_printf(_("%+.2f MB（%+.4f%%）"), mb, d);

            gtk_label_set_text(GTK_LABEL(s_delta), dt);
            g_free(dt);
        } else {
            gtk_label_set_text(GTK_LABEL(s_delta), _("采样中…"));
        }
    }

    gchar *err = g_strdup_printf(_("读 %llu / 写 %llu / CKSUM %llu"),
                                 (unsigned long long)h->read_err,
                                 (unsigned long long)h->write_err,
                                 (unsigned long long)h->cksum_err);

    gtk_label_set_text(GTK_LABEL(s_err), err);
    g_free(err);

    gchar *slow = g_strdup_printf(_("%llu 次"),
                                  (unsigned long long)h->slow_io);

    gtk_label_set_text(GTK_LABEL(s_slow), slow);
    g_free(slow);

    gchar *frag = (h->frag_pct >= 0)
                      ? g_strdup_printf(_("碎片率 %d %%"), h->frag_pct)
                      : g_strdup(_("碎片率 —"));

    gtk_label_set_text(GTK_LABEL(s_frag_h), frag);
    g_free(frag);

    if (h->has_txg) {
        gchar *txg;

        if (h->stime_ms >= 1000.0)
            txg = g_strdup_printf(
                _("txg %llu · stime %.2f s（近 %d 次均值 %.2f s）%s"),
                (unsigned long long)h->txg, h->stime_ms / 1000.0,
                h->txg_samples, h->txg_avg_ms / 1000.0,
                h->txg_spike ? _(" · 疑似脏数据节流（同步尖峰）") : "");
        else
            txg = g_strdup_printf(
                _("txg %llu · stime %.1f ms（近 %d 次均值 %.1f ms）%s"),
                (unsigned long long)h->txg, h->stime_ms, h->txg_samples,
                h->txg_avg_ms,
                h->txg_spike ? _(" · 疑似脏数据节流（同步尖峰）") : "");

        if (h->txg_qtime_ms > 0.0 || h->txg_wtime_ms > 0.0) {
            gchar *with_pipe = g_strdup_printf(
                _("%s · qtime %.1f ms · wtime %.1f ms（流水线）"), txg,
                h->txg_qtime_ms, h->txg_wtime_ms);

            g_free(txg);
            txg = with_pipe;
        }

        gtk_label_set_text(GTK_LABEL(s_txg), txg);
        g_free(txg);
    } else {
        gtk_label_set_text(GTK_LABEL(s_txg), _("采集中…"));
    }

    {
        gchar *dirty = g_strdup_printf(
            _("dirty_delay %llu · throttle %llu · wrlog_delay %llu"),
            (unsigned long long)h->dmu_dirty_delay,
            (unsigned long long)h->dmu_dirty_throttle,
            (unsigned long long)h->dmu_wrlog_delay);

        gtk_label_set_text(GTK_LABEL(s_dirty), dirty);
        g_free(dirty);
    }

    /* ARC 命中率（综合判断：ZFS 层是否被内存缓存挡住） */
    {
        const ZfsStats *zs = zfs_stats_get();
        gchar *arc;

        if (zs != NULL && zs->hits + zs->misses > 0)
            arc = g_strdup_printf(_("ARC 命中率 %.2f %%"),
                                  zfs_stats_hit_pct(zs->hits, zs->misses));
        else
            arc = g_strdup(_("ARC 命中率 —"));

        gtk_label_set_text(GTK_LABEL(s_arc_h), arc);
        g_free(arc);
    }

    gtk_label_set_text(GTK_LABEL(s_slog),
                       h->has_slog
                           ? (h->slog[0] != '\0' ? h->slog
                                                 : _("已配置（SLOG）"))
                           : _("未配置（ZIL 在池内）"));

    if (h->scan_pct[0] != '\0') {
        gchar *prog = g_strdup_printf(_("扫描中 %s%% · 剩余 %s"), h->scan_pct,
                                      h->scan_eta[0] ? h->scan_eta : "—");

        gtk_label_set_text(GTK_LABEL(s_scan), prog);
        g_free(prog);
    } else {
        gtk_label_set_text(GTK_LABEL(s_scan),
                           h->has_scan ? h->scan : _("没有 scrub 记录"));
    }

    rebuild_vdevs(h);

    guint64 total = h->read_err + h->write_err + h->cksum_err + h->slow_io;
    double  delta = 0.0;

    if (s_have_prev_err && total >= s_prev_err)
        delta = (double)(total - s_prev_err);

    s_prev_err      = total;
    s_have_prev_err = TRUE;

    s_err_hist[s_err_cd.index] = delta;
    s_err_cd.index = (s_err_cd.index + 1) % FMON_HISTORY_POINTS;

    if (s_err_cd.count < FMON_HISTORY_POINTS)
        s_err_cd.count++;

    s_err_cd.end_time_us = g_get_monotonic_time();
}

/* ===========================================================================
 * ARC 明细：三列布局
 * ========================================================================= */
static GtkWidget *s_arc_val[10];

static GtkWidget *arc_row(const char *key, GtkWidget *val)
{
    GtkWidget *h = gui_fixed_row(key, val, 78, 26);

    gtk_widget_set_margin_top(h, 2);
    return h;
}

GtkWidget *gui_arc_detail_box(void)
{
    static const char *const keys[10] = {
        "ARC 大小", "ARC 上限", "占上限", "目标 c", "数据缓冲",
        "元数据", "dnode", "dbuf", "系统可用", "内存节流",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_margin_top(box, 12);
    gtk_box_append(GTK_BOX(box), label_x(_("ARC 明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, 4);
    gtk_widget_set_size_request(c1, 300, -1);
    gtk_widget_set_size_request(c2, 300, -1);
    gtk_widget_set_hexpand(c3, TRUE);

    for (int i = 0; i < 10; i++) {
        s_arc_val[i] = label_x("—", "detail-value", 0.0f);

        GtkWidget *col = (i < 3) ? c1 : (i < 6) ? c2 : c3;

        gtk_box_append(GTK_BOX(col), arc_row(keys[i], s_arc_val[i]));
    }

    gtk_box_append(GTK_BOX(cols), c1);
    gtk_box_append(GTK_BOX(cols), c2);
    gtk_box_append(GTK_BOX(cols), c3);
    gtk_box_append(GTK_BOX(box), cols);
    return box;
}

/* ===========================================================================
 * ARC 构成趋势：数据缓冲 vs 元数据（界面内环形缓冲）
 * ========================================================================= */
static double     s_arc_data_hist[FMON_HISTORY_POINTS];
static double     s_arc_meta_hist[FMON_HISTORY_POINTS];
static CurveData  s_arc_data_cd = {
    .history = s_arc_data_hist,
    .unit    = "MB",
    .r = 0.25, .g = 0.65, .b = 0.95,
};
static CurveData  s_arc_meta_cd = {
    .history = s_arc_meta_hist,
    .unit    = "MB",
    .r = 0.95, .g = 0.60, .b = 0.20,
};
static ComboChart s_arc_trend;
static GtkWidget *s_arc_area = NULL;

static void arc_push(CurveData *cd, double v)
{
    ((double *)cd->history)[cd->index] = v;
    cd->index = (cd->index + 1) % FMON_HISTORY_POINTS;

    if (cd->count < FMON_HISTORY_POINTS)
        cd->count++;

    cd->end_time_us = g_get_monotonic_time();
    cd->interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
}

GtkWidget *gui_arc_trend_chart(void)
{
    s_arc_data_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
    s_arc_meta_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    s_arc_trend.n = 0;
    s_arc_trend.series[s_arc_trend.n++] =
        (ComboSeries){ "数据缓冲", &s_arc_data_cd, "" };
    s_arc_trend.series[s_arc_trend.n++] =
        (ComboSeries){ "元数据", &s_arc_meta_cd, "" };

    s_arc_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_arc_area, -1, 220);
    gtk_widget_set_hexpand(s_arc_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_arc_area),
                                   combo_chart_draw, &s_arc_trend, NULL);
    return s_arc_area;
}

void gui_arc_detail_update(void)
{
    if (s_arc_val[0] == NULL)
        return;

    const ZfsStats    *zs   = zfs_stats_get();
    const FmonShmData *snap = gui_state_snapshot();

    guint64 size = zs ? zs->size : snap->zfs_arc_size;
    guint64 max  = zs ? zs->c_max : snap->zfs_arc_c_max;

    char *s = fmon_gui_format_bytes(size);
    char *m = fmon_gui_format_bytes(max);

    gtk_label_set_text(GTK_LABEL(s_arc_val[0]), s);
    gtk_label_set_text(GTK_LABEL(s_arc_val[1]), m);
    g_free(s);
    g_free(m);

    gchar *pct = g_strdup_printf("%.1f %%",
                                 (max > 0) ? 100.0 * (double)size /
                                                 (double)max : 0.0);
    gtk_label_set_text(GTK_LABEL(s_arc_val[2]), pct);
    g_free(pct);

    if (zs == NULL) {
        for (int i = 3; i < 10; i++)
            gtk_label_set_text(GTK_LABEL(s_arc_val[i]), "—");
        return;
    }

    /* 数据缓冲/元数据用 MB 两位小数，和趋势图的读数完全对齐
     * （GB 只保留两位时会把 3228 MB 显示成 3.15 GB，看起来“不一致”） */
    char *vals[6] = {
        fmon_gui_format_bytes(zs->c),
        g_strdup_printf("%.2f MB", (double)zs->data_size / (1024.0 * 1024.0)),
        g_strdup_printf("%.2f MB",
                        (double)zs->metadata_size / (1024.0 * 1024.0)),
        fmon_gui_format_bytes(zs->dnode_size),
        fmon_gui_format_bytes(zs->dbuf_size),
        fmon_gui_format_bytes(snap->mem_avail_bytes),
    };

    for (int i = 0; i < 6; i++) {
        gtk_label_set_text(GTK_LABEL(s_arc_val[3 + i]), vals[i]);
        g_free(vals[i]);
    }

    gchar *thr = g_strdup_printf("%llu 次",
                                 (unsigned long long)zs->mem_throttle);

    gtk_label_set_text(GTK_LABEL(s_arc_val[9]), thr);
    g_free(thr);

    /* 构成趋势：GB 单位推入环形缓冲 */
    arc_push(&s_arc_data_cd, (double)zs->data_size / (1024.0 * 1024.0));
    arc_push(&s_arc_meta_cd,
             (double)zs->metadata_size / (1024.0 * 1024.0));

    if (s_arc_area != NULL)
        gtk_widget_queue_draw(s_arc_area);
}

/* ===========================================================================
 * 命中率明细：三列布局
 * ========================================================================= */
/* 近 60 秒滚动命中率：每秒存一次 hits/misses 增量，窗口内求和 */
static guint64    s_roll_dh[60], s_roll_dm[60];
static int        s_roll_pos, s_roll_cnt;
static double     s_roll_hist[FMON_HISTORY_POINTS];
static CurveData  s_roll_cd = {
    .history = s_roll_hist,
    .unit    = "%",
    .max_val = 100.0,
    .r = 0.20, .g = 0.70, .b = 0.90,
    .empty_hint = "开始采集后显示近 60 秒命中率",
    .min_val    = -1.0,
};
static GtkWidget *s_roll_area = NULL;

GtkWidget *gui_zhit_roll_chart(void)
{
    s_roll_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    s_roll_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_roll_area, -1, 180);
    gtk_widget_set_hexpand(s_roll_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_roll_area),
                                   gui_chart_draw, &s_roll_cd, NULL);
    return s_roll_area;
}

static GtkWidget *s_zhit_val[11];

GtkWidget *gui_zhit_detail_box(void)
{
    static const char *const keys[11] = {
        "总命中率", "数据命中率", "元数据命中率", "命中/秒",
        "预取命中率", "MRU 幽灵/秒", "MFU 幽灵/秒", "未命中/秒",
        "L2 命中率", "淘汰跳过", "哈希冲突",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_margin_top(box, 12);
    gtk_box_append(GTK_BOX(box), label_x(_("命中率明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, 4);
    gtk_widget_set_size_request(c1, 300, -1);
    gtk_widget_set_size_request(c2, 300, -1);
    gtk_widget_set_hexpand(c3, TRUE);

    for (int i = 0; i < 11; i++) {
        s_zhit_val[i] = label_x("—", "detail-value", 0.0f);

        GtkWidget *col = (i < 4) ? c1 : (i < 8) ? c2 : c3;

        gtk_box_append(GTK_BOX(col), arc_row(keys[i], s_zhit_val[i]));
    }

    gtk_box_append(GTK_BOX(cols), c1);
    gtk_box_append(GTK_BOX(cols), c2);
    gtk_box_append(GTK_BOX(cols), c3);
    gtk_box_append(GTK_BOX(box), cols);
    return box;
}

void gui_zhit_detail_update(void)
{
    static guint64 prev_mru, prev_mfu, prev_hits, prev_misses;
    static gint64  prev_us;

    if (s_zhit_val[0] == NULL)
        return;

    const ZfsStats *zs = zfs_stats_get();

    if (zs == NULL) {
        for (int i = 0; i < 11; i++)
            gtk_label_set_text(GTK_LABEL(s_zhit_val[i]), "—");
        return;
    }

    char b[96];

#define SETZ(i, ...)                                                    \
    do {                                                                \
        g_snprintf(b, sizeof(b), __VA_ARGS__);                          \
        gtk_label_set_text(GTK_LABEL(s_zhit_val[i]), b);                \
    } while (0)

    SETZ(0, "%.2f %%", zfs_stats_hit_pct(zs->hits, zs->misses));
    SETZ(1, "%.2f %%", zfs_stats_hit_pct(zs->demand_data_hits,
                                         zs->demand_data_misses));
    SETZ(2, "%.2f %%", zfs_stats_hit_pct(zs->demand_meta_hits,
                                         zs->demand_meta_misses));
    SETZ(4, "%.2f %%", zfs_stats_hit_pct(zs->prefetch_data_hits +
                                            zs->prefetch_meta_hits,
                                        zs->prefetch_data_misses +
                                            zs->prefetch_meta_misses));

    gint64 now = g_get_monotonic_time();
    double dt  = (prev_us > 0) ? (double)(now - prev_us) / 1e6 : 0.0;

    double hits_s   = (dt > 0.0) ? zfs_stats_rate(zs->hits, prev_hits, dt)
                                 : 0.0;
    double misses_s = (dt > 0.0) ? zfs_stats_rate(zs->misses, prev_misses, dt)
                                 : 0.0;

    guint64 dh = (zs->hits >= prev_hits) ? zs->hits - prev_hits : 0;
    guint64 dm = (zs->misses >= prev_misses) ? zs->misses - prev_misses : 0;

    SETZ(3, "%.0f 次/秒", hits_s);
    SETZ(7, "%.0f 次/秒", misses_s);

    SETZ(5, "%.1f 次/秒",
         (dt > 0.0) ? zfs_stats_rate(zs->mru_ghost_hits, prev_mru, dt) : 0.0);
    SETZ(6, "%.1f 次/秒",
         (dt > 0.0) ? zfs_stats_rate(zs->mfu_ghost_hits, prev_mfu, dt) : 0.0);

    prev_mru    = zs->mru_ghost_hits;
    prev_mfu    = zs->mfu_ghost_hits;
    prev_hits   = zs->hits;
    prev_misses = zs->misses;
    prev_us     = now;

    /* 60 秒滚动命中率 */
    s_roll_dh[s_roll_pos] = dh;
    s_roll_dm[s_roll_pos] = dm;
    s_roll_pos = (s_roll_pos + 1) % 60;

    if (s_roll_cnt < 60)
        s_roll_cnt++;

    guint64 sh = 0, sm = 0;

    for (int i = 0; i < s_roll_cnt; i++) {
        sh += s_roll_dh[i];
        sm += s_roll_dm[i];
    }

    double roll = (sh + sm > 0) ? 100.0 * (double)sh /
                                      (double)(sh + sm)
                                : 0.0;

    s_roll_hist[s_roll_cd.index] = roll;
    s_roll_cd.index = (s_roll_cd.index + 1) % FMON_HISTORY_POINTS;

    if (s_roll_cd.count < FMON_HISTORY_POINTS)
        s_roll_cd.count++;

    s_roll_cd.end_time_us = now;

    if (s_roll_area != NULL)
        gtk_widget_queue_draw(s_roll_area);

    if (zs->l2_hits + zs->l2_misses > 0)
        SETZ(8, "%.2f %%", zfs_stats_hit_pct(zs->l2_hits, zs->l2_misses));
    else
        SETZ(8, "%s", "无 L2");

    SETZ(9, "%llu 次", (unsigned long long)zs->evict_skip);
    SETZ(10, "%llu 次", (unsigned long long)zs->hash_collisions);

#undef SETZ
}

/* ===========================================================================
 * ZFS 带宽明细：三列 + 延迟直方图
 * ========================================================================= */
static GtkWidget *s_zbw_val[8];
static GtkWidget *s_zbw_lat;
static GtkWidget *s_zbw_req;
static GtkWidget *s_zbw_vdevs;

GtkWidget *gui_zbw_detail_box(void)
{
    static const char *const keys[8] = {
        "读取累计", "读取操作", "写入累计", "写入操作",
        "设备队列", "设备繁忙", "逻辑写入/秒", "写放大",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_margin_top(box, 12);
    gtk_box_append(GTK_BOX(box), label_x(_("带宽明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, 4);
    gtk_widget_set_size_request(c1, 300, -1);
    gtk_widget_set_size_request(c2, 300, -1);
    gtk_widget_set_hexpand(c3, TRUE);

    for (int i = 0; i < 8; i++) {
        s_zbw_val[i] = label_x("—", "detail-value", 0.0f);

        GtkWidget *col = (i < 3) ? c1 : (i < 6) ? c2 : c3;

        gtk_box_append(GTK_BOX(col), arc_row(keys[i], s_zbw_val[i]));
    }

    gtk_box_append(GTK_BOX(cols), c1);
    gtk_box_append(GTK_BOX(cols), c2);
    gtk_box_append(GTK_BOX(cols), c3);
    gtk_box_append(GTK_BOX(box), cols);

    s_zbw_lat = label_x("—", "group-desc", 0.0f);
    gtk_label_set_wrap(GTK_LABEL(s_zbw_lat), TRUE);
    gtk_widget_set_margin_top(s_zbw_lat, 4);
    gtk_box_append(GTK_BOX(box), s_zbw_lat);

    s_zbw_req = label_x("—", "group-desc", 0.0f);
    gtk_label_set_wrap(GTK_LABEL(s_zbw_req), TRUE);
    gtk_widget_set_margin_top(s_zbw_req, 2);
    gtk_box_append(GTK_BOX(box), s_zbw_req);

    s_zbw_vdevs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(s_zbw_vdevs, 2);
    gtk_box_append(GTK_BOX(box), s_zbw_vdevs);
    return box;
}

void gui_zbw_detail_update(void)
{
    if (s_zbw_val[0] == NULL)
        return;

    /* 带宽页自己触发 zpool 采样，否则只有访问“池空间”页才会更新 */
    zpool_health_update();

    const FmonShmData *snap = gui_state_snapshot();
    const FmonPoolInfo *z = (snap->pool_count > 0) ? &snap->pools[0] : NULL;
    const ZpoolHealth *h = zpool_health_get();

    guint64 rb = z ? z->read_bytes : 0;
    guint64 wb = z ? z->write_bytes : 0;

    char *rs = fmon_gui_format_bytes(rb);
    char *ws = fmon_gui_format_bytes(wb);

    gtk_label_set_text(GTK_LABEL(s_zbw_val[0]), rs);
    gtk_label_set_text(GTK_LABEL(s_zbw_val[2]), ws);
    g_free(rs);
    g_free(ws);

    char b[96];

    g_snprintf(b, sizeof(b), "%llu", (unsigned long long)(z ? z->read_ops : 0));
    gtk_label_set_text(GTK_LABEL(s_zbw_val[1]), b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)(z ? z->write_ops : 0));
    gtk_label_set_text(GTK_LABEL(s_zbw_val[3]), b);

    /* 设备队列长度（iostat -x 的 qlen）与繁忙度，替代内核不导出的 pend/activ */
    const DiskQueue *dq = disk_queue_get();

    if (dq != NULL) {
        GString *sq = g_string_new(NULL);

        for (int i = 0; i < dq->n && i < 2; i++)
            g_string_append_printf(sq, "%s%s %.0f", (i > 0) ? " · " : "",
                                   dq->dev[i].name, dq->dev[i].qlen);

        gtk_label_set_text(GTK_LABEL(s_zbw_val[4]), sq->str);
        g_string_free(sq, TRUE);

        g_snprintf(b, sizeof(b), "%.0f %%", dq->dev[0].busy_pct);
        gtk_label_set_text(GTK_LABEL(s_zbw_val[5]), b);
    } else {
        gtk_label_set_text(GTK_LABEL(s_zbw_val[4]), "—");
        gtk_label_set_text(GTK_LABEL(s_zbw_val[5]), "—");
    }

    /* 逻辑写速率（数据集 kstat，无 zfs list 开销）与写放大 */
    static guint64 prev_written, prev_phys;
    static gint64  prev_us;

    if (h != NULL && h->ds_written > 0) {
        gint64 now = g_get_monotonic_time();

        if (prev_us == 0) {
            prev_written = h->ds_written;
            prev_us      = now;
        } else if (h->ds_written > prev_written) {
            /* 只在 kstat 更新（15 秒一次）时计算，避免每秒把差值摊薄为 0 */
            double dt = (double)(now - prev_us) / 1e6;
            double lw = (dt > 0.0)
                            ? (double)(h->ds_written - prev_written) / dt
                            : 0.0;

            prev_written = h->ds_written;
            prev_us      = now;

            char *ls = fmon_gui_format_rate(lw / 1024.0);

            gtk_label_set_text(GTK_LABEL(s_zbw_val[6]), ls);
            g_free(ls);

            /* 物理写速率用池累计写字节的同一时间窗口差分，避免
             * “1 秒瞬时 / 15 秒平均”混算导致放大比失真 */
            guint64 pw = (snap->pool_count > 0) ? snap->pools[0].write_bytes : 0;
            double phys = (dt > 0.0 && pw >= prev_phys)
                              ? (double)(pw - prev_phys) / dt
                              : 0.0;

            prev_phys = pw;

            if (lw > 100.0 * 1024.0 && phys > 0.0)
                g_snprintf(b, sizeof(b), "%.2fx", phys / lw);
            else
                g_snprintf(b, sizeof(b), _("—（当前写入过低）"));

            gtk_label_set_text(GTK_LABEL(s_zbw_val[7]), b);
        }
    }

    /* 延迟直方图：只显示计数 > 0 的桶，避免一行太长 */
    GString *g = g_string_new(_("延迟直方图  "));

    if (h != NULL) {
        for (int i = 0; i < h->lat_n; i++) {
            if (h->lat[i].count <= 0.0)
                continue;

            g_string_append_printf(g, "%s %.0f   ", h->lat[i].label,
                                   h->lat[i].count);
        }
    }

    if (g->len <= strlen(_("延迟直方图  ")))
        g_string_append(g, _("无数据（空闲）"));

    gtk_label_set_text(GTK_LABEL(s_zbw_lat), g->str);
    g_string_free(g, TRUE);

    /* 每个 vdev 的延迟（替代不可用的 vdev 队列）；内容不变就不重建 */
    static gchar *s_zbw_vdev_sig;

    if (s_zbw_vdevs != NULL) {
        GString *sig = g_string_new(NULL);

        if (h != NULL) {
            for (int i = 0; i < h->vdev_count; i++)
                g_string_append_printf(sig, "%s|%.2f|%.2f|%.2f|%.2f;",
                                       h->vdev[i].name, h->vdev[i].tw_r_ms,
                                       h->vdev[i].tw_w_ms, h->vdev[i].dw_r_ms,
                                       h->vdev[i].dw_w_ms);
        }

        if (g_strcmp0(sig->str, s_zbw_vdev_sig) == 0) {
            g_string_free(sig, TRUE);
            goto vdev_done;
        }

        g_free(s_zbw_vdev_sig);
        s_zbw_vdev_sig = g_strdup(sig->str);
        g_string_free(sig, TRUE);

        GtkWidget *child = NULL;

        while ((child = gtk_widget_get_first_child(s_zbw_vdevs)) != NULL)
            gtk_box_remove(GTK_BOX(s_zbw_vdevs), child);

        if (h != NULL && h->vdev_count > 0) {
            for (int i = 0; i < h->vdev_count; i++) {
                const char *nm = h->vdev[i].name;

                gchar *txt = g_strdup_printf(
                    _("%s  延迟 读 %.2f / 写 %.2f ms · disk 读 %.2f / 写 %.2f ms"),
                    nm, h->vdev[i].tw_r_ms, h->vdev[i].tw_w_ms,
                    h->vdev[i].dw_r_ms, h->vdev[i].dw_w_ms);

                gtk_box_append(GTK_BOX(s_zbw_vdevs),
                               label_x(txt, "group-desc", 0.0f));
                g_free(txt);
            }
        } else {
            gtk_box_append(GTK_BOX(s_zbw_vdevs),
                           label_x(_("vdev 延迟：采集中…"), "group-desc",
                                   0.0f));
        }
    }

vdev_done:
    /* 写请求大小分布（iostat -r 聚合） */
    if (h != NULL && s_zbw_req != NULL) {
        gchar *rq = g_strdup_printf(
            _("写请求大小  ≤4K %.0f · 8–64K %.0f · ≥128K %.0f"),
            h->req_small, h->req_mid, h->req_large);

        gtk_label_set_text(GTK_LABEL(s_zbw_req), rq);
        g_free(rq);
    }
}







/* ===========================================================================
 * ARC Prune 回收明细：三列 + 回收风暴判据
 * ========================================================================= */
static GtkWidget *s_prune_val[12];
static GtkWidget *s_prune_status;
static GtkWidget *s_prune_l2;

GtkWidget *gui_prune_detail_box(void)
{
    static const char *const keys[12] = {
        "prune/秒", "跳过/秒", "回收不足/秒", "删除回收",
        "节流/秒", "no_grow", "空闲内存", "direct 回收/秒",
        "可回收比例", "幽灵占比", "哈希元素", "哈希碰撞",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_margin_top(box, 12);
    gtk_box_append(GTK_BOX(box), label_x(_("回收明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, 4);
    gtk_widget_set_size_request(c1, 300, -1);
    gtk_widget_set_size_request(c2, 300, -1);
    gtk_widget_set_hexpand(c3, TRUE);

    for (int i = 0; i < 12; i++) {
        s_prune_val[i] = label_x("—", "detail-value", 0.0f);

        GtkWidget *col = (i < 4) ? c1 : (i < 8) ? c2 : c3;

        gtk_box_append(GTK_BOX(col), gui_fixed_row(keys[i], s_prune_val[i],
                                                   78, 26));
    }

    gtk_box_append(GTK_BOX(cols), c1);
    gtk_box_append(GTK_BOX(cols), c2);
    gtk_box_append(GTK_BOX(cols), c3);
    gtk_box_append(GTK_BOX(box), cols);

    s_prune_status = label_x("—", "group-desc", 0.0f);
    gtk_widget_set_margin_top(s_prune_status, 4);
    gtk_box_append(GTK_BOX(box), s_prune_status);

    s_prune_l2 = label_x("—", "group-desc", 0.0f);
    gtk_widget_set_margin_top(s_prune_l2, 2);
    gtk_box_append(GTK_BOX(box), s_prune_l2);
    return box;
}

/* ===========================================================================
 * 幽灵缓存趋势（MRU / MFU ghost size）
 * ========================================================================= */
static double     s_ghost_mru_hist[FMON_HISTORY_POINTS];
static double     s_ghost_mfu_hist[FMON_HISTORY_POINTS];
static CurveData  s_ghost_mru_cd = {
    .history = s_ghost_mru_hist,
    .unit    = "MB",
    .r = 0.55, .g = 0.35, .b = 0.95,
};
static CurveData  s_ghost_mfu_cd = {
    .history = s_ghost_mfu_hist,
    .unit    = "MB",
    .r = 0.95, .g = 0.55, .b = 0.25,
};
static ComboChart s_ghost_chart;
static GtkWidget *s_ghost_area = NULL;

GtkWidget *gui_prune_ghost_chart(void)
{
    s_ghost_mru_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
    s_ghost_mfu_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    s_ghost_chart.n = 0;
    s_ghost_chart.series[s_ghost_chart.n++] =
        (ComboSeries){ "MRU 幽灵", &s_ghost_mru_cd, "" };
    s_ghost_chart.series[s_ghost_chart.n++] =
        (ComboSeries){ "MFU 幽灵", &s_ghost_mfu_cd, "" };

    s_ghost_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_ghost_area, -1, 200);
    gtk_widget_set_hexpand(s_ghost_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_ghost_area),
                                   combo_chart_draw, &s_ghost_chart, NULL);
    return s_ghost_area;
}

static void ghost_push(CurveData *cd, double v)
{
    ((double *)cd->history)[cd->index] = v;
    cd->index = (cd->index + 1) % FMON_HISTORY_POINTS;

    if (cd->count < FMON_HISTORY_POINTS)
        cd->count++;

    cd->end_time_us = g_get_monotonic_time();
    cd->interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
}

void gui_prune_detail_update(void)
{
    static guint64 prev_prune, prev_skip, prev_not_enough;
    static guint64 prev_throttle, prev_direct;
    static gint64  prev_us;

    if (s_prune_val[0] == NULL)
        return;

    const ZfsStats *zs = zfs_stats_get();

    if (zs == NULL) {
        for (int i = 0; i < 12; i++)
            gtk_label_set_text(GTK_LABEL(s_prune_val[i]), "—");
        gtk_label_set_text(GTK_LABEL(s_prune_status), _("无 ZFS 数据"));
        return;
    }

    gint64 now = g_get_monotonic_time();
    double dt  = (prev_us > 0) ? (double)(now - prev_us) / 1e6 : 0.0;

#define RATE(cur, prev)                                                         ((dt > 0.0) ? zfs_stats_rate((cur), (prev), dt) : 0.0)

    double prune_s    = RATE(zs->arc_prune, prev_prune);
    double skip_s     = RATE(zs->evict_skip, prev_skip);
    double noten_s    = RATE(zs->evict_not_enough, prev_not_enough);
    double throttle_s = RATE(zs->mem_throttle, prev_throttle);
    double direct_s   = RATE(zs->mem_direct, prev_direct);
    guint64 throttle_delta = (zs->mem_throttle >= prev_throttle)
                                 ? zs->mem_throttle - prev_throttle
                                 : 0;

    char b[96];

    g_snprintf(b, sizeof(b), "%.1f 次/秒", prune_s);
    gtk_label_set_text(GTK_LABEL(s_prune_val[0]), b);
    g_snprintf(b, sizeof(b), "%.1f 次/秒", skip_s);
    gtk_label_set_text(GTK_LABEL(s_prune_val[1]), b);
    g_snprintf(b, sizeof(b), "%.1f 次/秒", noten_s);
    gtk_label_set_text(GTK_LABEL(s_prune_val[2]), b);
    g_snprintf(b, sizeof(b), "%llu", (unsigned long long)zs->deleted);
    gtk_label_set_text(GTK_LABEL(s_prune_val[3]), b);

    g_snprintf(b, sizeof(b), "%.1f 次/秒", throttle_s);
    gtk_label_set_text(GTK_LABEL(s_prune_val[4]), b);
    g_snprintf(b, sizeof(b), "%llu", (unsigned long long)zs->no_grow);
    gtk_label_set_text(GTK_LABEL(s_prune_val[5]), b);

    char *freeb = fmon_gui_format_bytes(zs->mem_free);
    gtk_label_set_text(GTK_LABEL(s_prune_val[6]), freeb);
    g_free(freeb);

    g_snprintf(b, sizeof(b), "%.1f 次/秒", direct_s);
    gtk_label_set_text(GTK_LABEL(s_prune_val[7]), b);

    guint64 evictable = zs->mru_evictable_data + zs->mru_evictable_meta +
                        zs->mfu_evictable_data + zs->mfu_evictable_meta +
                        zs->anon_evictable_data + zs->anon_evictable_meta;
    guint64 total = zs->mru_size + zs->mfu_size + zs->anon_size;

    g_snprintf(b, sizeof(b), "%.0f %%",
               (total > 0) ? 100.0 * (double)evictable / (double)total : 0.0);
    gtk_label_set_text(GTK_LABEL(s_prune_val[8]), b);

    guint64 ghost = zs->mru_ghost_size + zs->mfu_ghost_size;

    g_snprintf(b, sizeof(b), "%.1f %%",
               (zs->size > 0) ? 100.0 * (double)ghost / (double)zs->size
                              : 0.0);
    gtk_label_set_text(GTK_LABEL(s_prune_val[9]), b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)zs->hash_elements);
    gtk_label_set_text(GTK_LABEL(s_prune_val[10]), b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)zs->hash_collisions);
    gtk_label_set_text(GTK_LABEL(s_prune_val[11]), b);

    /* 回收风暴判据：prune 或 evict_skip 突增，且内存节流在同窗口内增长 */
    ZfsReclaimState st = zfs_stats_reclaim_state(prune_s, skip_s,
                                                 throttle_delta,
                                                 zs->no_grow);

    if (st == ZFS_RECLAIM_PRESSURE)
        g_snprintf(b, sizeof(b),
                   _("状态：内存压力回收（prune %.1f/s · skip %.1f/s · 节流 +%llu）"),
                   prune_s, skip_s, (unsigned long long)throttle_delta);
    else if (st == ZFS_RECLAIM_NORMAL)
        g_snprintf(b, sizeof(b), _("状态：正常回收（prune %.1f/s · skip %.1f/s）"),
                   prune_s, skip_s);
    else
        g_snprintf(b, sizeof(b), _("状态：空闲（无回收活动）"));

    gtk_label_set_text(GTK_LABEL(s_prune_status), b);

    /* L2ARC 相关（无 L2 时明确说明） */
    if (zs->l2_size > 0)
        g_snprintf(b, sizeof(b),
                   _("L2ARC 淘汰跳过 %llu · 淘汰读 %llu · free_on_write %llu · 写错误 %llu"),
                   (unsigned long long)zs->l2_evict_l1cached,
                   (unsigned long long)zs->l2_evict_reading,
                   (unsigned long long)zs->l2_free_on_write,
                   (unsigned long long)zs->l2_writes_error);
    else
        g_snprintf(b, sizeof(b), _("L2ARC：未配置"));

    gtk_label_set_text(GTK_LABEL(s_prune_l2), b);

    /* 幽灵缓存趋势（MB） */
    ghost_push(&s_ghost_mru_cd,
               (double)zs->mru_ghost_size / (1024.0 * 1024.0));
    ghost_push(&s_ghost_mfu_cd,
               (double)zs->mfu_ghost_size / (1024.0 * 1024.0));

    if (s_ghost_area != NULL)
        gtk_widget_queue_draw(s_ghost_area);

    prev_prune      = zs->arc_prune;
    prev_skip       = zs->evict_skip;
    prev_not_enough = zs->evict_not_enough;
    prev_throttle   = zs->mem_throttle;
    prev_direct     = zs->mem_direct;
    prev_us         = now;

#undef RATE
}














