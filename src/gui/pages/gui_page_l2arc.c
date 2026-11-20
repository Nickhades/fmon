/*
 * gui_page_l2arc.c - L2ARC（二级读缓存）明细
 *
 * 数据来自 zfs_stats（kstat.zfs.misc.arcstats.*），每秒刷新。
 * 未配置 L2ARC 时页面明确提示，而不是显示一堆 0。
 */

#include "gui_page_l2arc.h"

#include "zfs_stats.h"

#include "gui_chart.h"
#include "gui_state.h"
#include "gui_util.h"
#include "gui_widgets.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

enum {
    L2_ROW_DEVS = 0,
    L2_ROW_SIZE,
    L2_ROW_ASIZE,
    L2_ROW_HDR,
    L2_ROW_HIT,
    L2_ROW_HITS,
    L2_ROW_READ,
    L2_ROW_WRITE,
    L2_ROW_WRITES,
    L2_ROW_ERRORS,
    L2_ROW_EVICT,
    L2_ROW_REBUILD,
    L2_ROW_LAYOUT,
    L2_ROW_ABORT,
    L2_ROW_OTHER,
    L2_ROW_COUNT
};

static const char *const L2_KEYS[L2_ROW_COUNT] = {
    "L2 设备", "缓存大小", "实际分配", "头部开销",
    "命中率", "命中 / 未命中", "读速率", "写速率",
    "写入完成", "错误", "驱逐", "重建",
    "构成（MFU/MRU/预取）", "低内存中止", "其他",
};

static GtkWidget *s_vals[L2_ROW_COUNT];
static GtkWidget *s_hint = NULL;

/* 历史曲线来自 daemon：zfs_l2_size_history / zfs_l2_hit_history（ABI 23）。
 * 下面两片数组只在首帧、daemon 还没给指针之前兜底。 */
static double s_size_hist[FMON_HISTORY_POINTS];
static double s_hit_hist[FMON_HISTORY_POINTS];

static CurveData s_size_cd = {
    .history = s_size_hist, .unit = "GB",
    .r = 0.22, .g = 0.72, .b = 0.85,
    .empty_hint = "配置 L2ARC 后开始采集",
};
static CurveData s_hit_cd = {
    .history = s_hit_hist, .unit = "%", .max_val = 100.0,
    .r = 0.95, .g = 0.70, .b = 0.25,
    .empty_hint = "配置 L2ARC 后开始采集",
};

static GtkWidget *s_size_area = NULL;
static GtkWidget *s_hit_area  = NULL;
static GtkWidget *s_size_now  = NULL;
static GtkWidget *s_hit_now   = NULL;

static void sync_curve(CurveData *cd, const FmonShmData *snap,
                       const double *history)
{
    cd->history       = (history != NULL) ? history : cd->history;
    cd->count         = snap->history_count;
    cd->index         = snap->history_index;
    cd->end_time_us   = snap->last_update_us;
    cd->time_us       = snap->history_time_us;
    cd->interval_us   = (snap->history_interval_us > 0)
                            ? snap->history_interval_us
                            : (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
}

static GtkWidget *chart_card(const char *title, CurveData *cd,
                             GtkWidget **out_area, GtkWidget **out_now)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_widget_add_css_class(card, "page-card");
    gtk_widget_set_margin_top(card, 6);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *t    = gtk_label_new(title);

    gtk_widget_add_css_class(t, "detail-title");
    gtk_widget_set_hexpand(t, TRUE);
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);

    *out_now = gtk_label_new("—");
    gtk_widget_add_css_class(*out_now, "detail-value");
    gtk_label_set_xalign(GTK_LABEL(*out_now), 1.0f);

    gtk_box_append(GTK_BOX(head), t);
    gtk_box_append(GTK_BOX(head), *out_now);
    gtk_box_append(GTK_BOX(card), head);

    *out_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(*out_area, -1, 200);
    gtk_widget_set_hexpand(*out_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(*out_area),
                                   gui_chart_draw, cd, NULL);

    gtk_box_append(GTK_BOX(card), *out_area);
    return card;
}

static void setv(int i, const char *text)
{
    if (i >= 0 && i < L2_ROW_COUNT && s_vals[i] != NULL)
        gtk_label_set_text(GTK_LABEL(s_vals[i]), text);
}

GtkWidget *gui_l2arc_page_new(void)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);

    GtkWidget *t = gtk_label_new(_("L2ARC"));
    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);

    GtkWidget *s = gtk_label_new(_("二级读缓存（SSD 上的 ARC 扩展）"));
    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);

    s_hint = gtk_label_new(_("未配置 L2ARC：当前系统没有 L2ARC 设备"
                             "（vfs.zfs.l2arc_* / 缓存盘），以下指标为空。"));
    gtk_widget_add_css_class(s_hint, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s_hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(s_hint), TRUE);
    gtk_widget_set_visible(s_hint, FALSE);

    GtkWidget *grid = NULL;
    GtkWidget *card = gui_info_card(_("L2ARC 明细"), &grid);

    for (int i = 0; i < L2_ROW_COUNT; i++) {
        gtk_grid_attach(GTK_GRID(grid),
                        gui_make_cell(_(L2_KEYS[i]), "detail-key"), 0, i, 1,
                        1);

        s_vals[i] = gui_make_cell("—", "detail-value");
        gtk_widget_set_hexpand(s_vals[i], TRUE);
        gtk_grid_attach(GTK_GRID(grid), s_vals[i], 1, i, 1, 1);
    }

    gtk_box_append(GTK_BOX(vbox), t);
    gtk_box_append(GTK_BOX(vbox), s);
    gtk_box_append(GTK_BOX(vbox), s_hint);

    gtk_box_append(GTK_BOX(vbox),
                   chart_card(_("L2 缓存大小"), &s_size_cd, &s_size_area,
                              &s_size_now));
    gtk_box_append(GTK_BOX(vbox),
                   chart_card(_("L2 命中率"), &s_hit_cd, &s_hit_area,
                              &s_hit_now));

    /* 提示和卡片之间留一点距离 */
    gtk_widget_set_margin_top(card, 6);
    gtk_box_append(GTK_BOX(vbox), card);

    return vbox;
}

void gui_l2arc_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();
    const ZfsStats    *zs   = zfs_stats_get();
    gboolean has_l2 = (snap != NULL && snap->zfs_l2_size > 0);

    if (s_hint != NULL)
        gtk_widget_set_visible(s_hint, !has_l2);

    if (!has_l2) {
        for (int i = 0; i < L2_ROW_COUNT; i++)
            setv(i, "—");

        if (s_size_now != NULL)
            gtk_label_set_text(GTK_LABEL(s_size_now), "—");
        if (s_hit_now != NULL)
            gtk_label_set_text(GTK_LABEL(s_hit_now), "—");
        return;
    }

    /* 历史曲线由 daemon 发布（ABI 23）：大小 GB 与命中率 % */
    sync_curve(&s_size_cd, snap, snap->zfs_l2_size_history);
    sync_curve(&s_hit_cd, snap, snap->zfs_l2_hit_history);

    double size_gb = (double)snap->zfs_l2_size / (1024.0 * 1024.0 * 1024.0);
    double hit_pct = (snap->zfs_l2_hits + snap->zfs_l2_misses > 0)
                         ? 100.0 * (double)snap->zfs_l2_hits /
                               (double)(snap->zfs_l2_hits + snap->zfs_l2_misses)
                         : 0.0;

    gchar *now = g_strdup_printf("%.2f GB", size_gb);

    gtk_label_set_text(GTK_LABEL(s_size_now), now);
    g_free(now);

    now = g_strdup_printf("%.2f %%", hit_pct);
    gtk_label_set_text(GTK_LABEL(s_hit_now), now);
    g_free(now);

    gtk_widget_queue_draw(s_size_area);
    gtk_widget_queue_draw(s_hit_area);

    char b[128];

    g_snprintf(b, sizeof(b), "%llu 块", (unsigned long long)zs->l2_ndev);
    setv(L2_ROW_DEVS, b);

    char *size  = fmon_gui_format_bytes(zs->l2_size);
    char *asize = fmon_gui_format_bytes(zs->l2_asize);
    char *hdr   = fmon_gui_format_bytes(zs->l2_hdr_size);

    setv(L2_ROW_SIZE, size);
    setv(L2_ROW_ASIZE, asize);
    setv(L2_ROW_HDR, hdr);

    if (zs->l2_hits + zs->l2_misses > 0)
        g_snprintf(b, sizeof(b), "%.2f %%",
                   zfs_stats_hit_pct(zs->l2_hits, zs->l2_misses));
    else
        g_snprintf(b, sizeof(b), "—（尚无访问）");
    setv(L2_ROW_HIT, b);

    g_snprintf(b, sizeof(b), "%llu / %llu",
               (unsigned long long)zs->l2_hits,
               (unsigned long long)zs->l2_misses);
    setv(L2_ROW_HITS, b);

    char *rd = fmon_gui_format_bytes((guint64)zs->l2_read_bps);
    char *wr = fmon_gui_format_bytes((guint64)zs->l2_write_bps);

    g_snprintf(b, sizeof(b), "%s/s", rd);
    setv(L2_ROW_READ, b);
    g_snprintf(b, sizeof(b), "%s/s", wr);
    setv(L2_ROW_WRITE, b);

    g_free(rd);
    g_free(wr);

    double done_pct = (zs->l2_writes_sent > 0)
                          ? 100.0 * (double)zs->l2_writes_done /
                                (double)zs->l2_writes_sent
                          : 0.0;

    g_snprintf(b, sizeof(b), "%.1f %%（%llu / %llu）", done_pct,
               (unsigned long long)zs->l2_writes_done,
               (unsigned long long)zs->l2_writes_sent);
    setv(L2_ROW_WRITES, b);

    g_snprintf(b, sizeof(b), "写 %llu · IO %llu · 校验 %llu",
               (unsigned long long)zs->l2_writes_error,
               (unsigned long long)zs->l2_io_error,
               (unsigned long long)zs->l2_cksum_bad);
    setv(L2_ROW_ERRORS, b);

    g_snprintf(b, sizeof(b), "读取中 %llu · L1 已缓存 %llu",
               (unsigned long long)zs->l2_evict_reading,
               (unsigned long long)zs->l2_evict_l1cached);
    setv(L2_ROW_EVICT, b);

    char *rebuild = fmon_gui_format_bytes(zs->l2_rebuild_size);

    g_snprintf(b, sizeof(b), "成功 %llu 次 · %s（IO 错误 %llu）",
               (unsigned long long)zs->l2_rebuild_success, rebuild,
               (unsigned long long)zs->l2_rebuild_io_errors);
    setv(L2_ROW_REBUILD, b);
    g_free(rebuild);

    char *mfu = fmon_gui_format_bytes(zs->l2_mfu_asize);
    char *mru = fmon_gui_format_bytes(zs->l2_mru_asize);
    char *pre = fmon_gui_format_bytes(zs->l2_prefetch_asize);

    g_snprintf(b, sizeof(b), "%s / %s / %s", mfu, mru, pre);
    setv(L2_ROW_LAYOUT, b);

    g_free(mfu);
    g_free(mru);
    g_free(pre);

    g_snprintf(b, sizeof(b), "%llu 次",
               (unsigned long long)zs->l2_abort_lowmem);
    setv(L2_ROW_ABORT, b);

    g_snprintf(b, sizeof(b), "feeds %llu · 读写冲突 %llu",
               (unsigned long long)zs->l2_feeds,
               (unsigned long long)zs->l2_rw_clash);
    setv(L2_ROW_OTHER, b);

    g_free(size);
    g_free(asize);
    g_free(hdr);
}
