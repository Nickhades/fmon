/*
 * gui_zfs_arc.c - ZFS ARC 明细与构成趋势
 */

#include "gui_zfs_arc.h"

#include "ui_metrics.h"
#include "gui_state.h"
#include "gui_util.h"
#include "gui_widgets.h"
#include "combo_chart.h"
#include "zfs_stats.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

static GtkWidget *label_x(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_widget_set_halign(l, (xalign >= 1.0f) ? GTK_ALIGN_END : GTK_ALIGN_START);
    return l;
}

static GtkWidget *s_arc_val[10];

static GtkWidget *arc_row(const char *key, GtkWidget *val)
{
    GtkWidget *h = gui_fixed_row(key, val, 78, 26);

    gtk_widget_set_margin_top(h, FMON_SPACE_1);
    return h;
}

GtkWidget *gui_arc_detail_box(void)
{
    static const char *const keys[10] = {
        "ARC 大小", "ARC 上限", "占上限", "目标 c", "数据缓冲",
        "元数据", "dnode", "dbuf", "系统可用", "内存节流",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);
    gtk_box_append(GTK_BOX(box), label_x(_("ARC 明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_8);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, FMON_SPACE_1);
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
static uint64_t   s_arc_data_time[FMON_HISTORY_POINTS];
static uint64_t   s_arc_meta_time[FMON_HISTORY_POINTS];
static CurveData  s_arc_data_cd = {
    .history = s_arc_data_hist,
    .time_us = s_arc_data_time,
    .unit    = "MB",
    .r = 0.00, .g = 1.00, .b = 1.00,
};
static CurveData  s_arc_meta_cd = {
    .history = s_arc_meta_hist,
    .time_us = s_arc_meta_time,
    .unit    = "MB",
    .r = 1.00, .g = 1.00, .b = 0.00,
};
static ComboChart s_arc_trend;
static GtkWidget *s_arc_area = NULL;

static void arc_push(CurveData *cd, double v)
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
    gui_chart_attach_interactions(
        s_arc_area, s_arc_trend.n > 0 ? s_arc_trend.series[0].cd : NULL);
    return s_arc_area;
}

void gui_arc_detail_update(void)
{
    if (s_arc_val[0] == NULL || !gtk_widget_get_mapped(s_arc_val[0]))
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
