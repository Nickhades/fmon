/*
 * gui_zfs_zpool.c - ZFS 池摘要、详情、vdev、错误趋势
 */

#include "gui_zfs_zpool.h"

#include "ui_metrics.h"
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
static uint64_t  s_err_time[FMON_HISTORY_POINTS];
static CurveData s_err_cd = {
    .history = s_err_hist,
    .time_us = s_err_time,
    .unit    = "",
    .r = 1.00, .g = 0.00, .b = 0.00,
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
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

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
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);
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
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_8);
    GtkWidget *lgrid = gtk_grid_new();
    GtkWidget *rgrid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(lgrid), FMON_SPACE_1);
    gtk_grid_set_column_spacing(GTK_GRID(lgrid), FMON_SPACE_3);
    gtk_grid_set_row_spacing(GTK_GRID(rgrid), FMON_SPACE_1);
    gtk_grid_set_column_spacing(GTK_GRID(rgrid), FMON_SPACE_3);
    gtk_widget_set_margin_top(cols, FMON_SPACE_1);

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

    s_vdev_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);
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
    } else {
        gtk_label_set_text(GTK_LABEL(s_pool),
                           ((snap->probe_valid & FMON_PROBE_POOLS) == 0)
                               ? _("zpool 不可用")
                               : _("无可用池"));
        gtk_label_set_text(GTK_LABEL(s_cap), "—");
        gtk_label_set_text(GTK_LABEL(s_pct), "—");
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
            double v_last = ss->zpool_util_history[last];
            double v_old  = ss->zpool_util_history[old];

            if (!isfinite(v_last) || !isfinite(v_old)) {
                gtk_label_set_text(GTK_LABEL(s_delta), "—");
            } else {
                double d  = v_last - v_old;
                double mb = d / 100.0 * (double)z->total_bytes /
                            (1024.0 * 1024.0);

                gchar *dt = g_strdup_printf(_("%+.2f MB（%+.4f%%）"), mb, d);

                gtk_label_set_text(GTK_LABEL(s_delta), dt);
                g_free(dt);
            }
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
    s_err_time[s_err_cd.index] = g_get_monotonic_time();
    s_err_cd.index = (s_err_cd.index + 1) % FMON_HISTORY_POINTS;

    if (s_err_cd.count < FMON_HISTORY_POINTS)
        s_err_cd.count++;

    s_err_cd.end_time_us = g_get_monotonic_time();
}

/* ===========================================================================
 * ARC 明细：三列布局
 * ========================================================================= */
