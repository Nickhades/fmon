/*
 * gui_page_prune.c - ARC Prune 明细与幽灵缓存趋势
 */

#include "gui_page_prune.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "combo_chart.h"
#include "gui_chart.h"
#include "gui_util.h"
#include "gui_widgets.h"
#include "ui_metrics.h"
#include "zfs_stats.h"

static GtkWidget *label_x(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_widget_set_halign(l, (xalign >= 1.0f) ? GTK_ALIGN_END : GTK_ALIGN_START);
    return l;
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

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);
    gtk_box_append(GTK_BOX(box), label_x(_("回收明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_8);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, FMON_SPACE_1);
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
    gtk_widget_set_margin_top(s_prune_status, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_prune_status);

    s_prune_l2 = label_x("—", "group-desc", 0.0f);
    gtk_widget_set_margin_top(s_prune_l2, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_prune_l2);

    /* 这是事件累计值，不是“有没有采集到”的标记：空闲时正常就是 0。 */
    GtkWidget *note = label_x(
        _("说明：ARC Prune 是事件累计计数；无内存压力、没有 ARC 驱逐时"
          "保持 0 属于正常，不需要配置。"), "small-text", 0.0f);

    gtk_widget_set_margin_top(note, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), note);
    return box;
}

/* ===========================================================================
 * 幽灵缓存趋势（MRU / MFU ghost size）
 * ========================================================================= */
static double     s_ghost_mru_hist[FMON_HISTORY_POINTS];
static double     s_ghost_mfu_hist[FMON_HISTORY_POINTS];
static uint64_t   s_ghost_mru_time[FMON_HISTORY_POINTS];
static uint64_t   s_ghost_mfu_time[FMON_HISTORY_POINTS];
static CurveData  s_ghost_mru_cd = {
    .history = s_ghost_mru_hist,
    .time_us = s_ghost_mru_time,
    .unit    = "MB",
    .r = 1.00, .g = 0.00, .b = 1.00,
};
static CurveData  s_ghost_mfu_cd = {
    .history = s_ghost_mfu_hist,
    .time_us = s_ghost_mfu_time,
    .unit    = "MB",
    .r = 0.00, .g = 1.00, .b = 1.00,
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
    gui_chart_attach_interactions(
        s_ghost_area,
        s_ghost_chart.n > 0 ? s_ghost_chart.series[0].cd : NULL);
    return s_ghost_area;
}

static void ghost_push(CurveData *cd, double v)
{
    gint64 now = g_get_monotonic_time();

    ((double *)cd->history)[cd->index] = v;
    if (cd->time_us != NULL)
        ((uint64_t *)cd->time_us)[cd->index] = (uint64_t)now;

    cd->index = (cd->index + 1) % FMON_HISTORY_POINTS;

    if (cd->count < FMON_HISTORY_POINTS)
        cd->count++;

    cd->end_time_us = (uint64_t)now;
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
