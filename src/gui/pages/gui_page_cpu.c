/*
 * gui_page_cpu.c - CPU 页扩展：时间分布堆叠条、频率、调度状态（对齐网格）
 *
 * 每核占用由 gui_coreview 的方块展示；这里用统一的键值网格补齐
 * 时间构成、频率与调度压力，避免多行文字宽度不一造成的参差。
 */

#include "gui_page_cpu.h"
#include "gui_cpu_detail.h"
#include "gui_cpu_gauge.h"
#include "cpu_curve_util.h"
#include "cpu_chart_util.h"
#include "gui_cpu_temp_chart.h"
#include "ui_metrics.h"

#include "chart_geom.h"
#include "combo_chart.h"
#include "cpu_health.h"
#include "cpu_stats.h"
#include "gui_chart.h"
#include "gui_chart_util.h"
#include "alerts.h"
#include "palette.h"
#include "gui_pages.h"
#include "gui_state.h"
#include "theme.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>
#include <stdlib.h>

static GtkWidget *s_avg_area;
static GtkWidget *s_temp_area;

typedef struct {
    const CurveData *cd;
    const double    *history;
    const uint64_t  *time_us;
    uint32_t count;
    uint32_t index;
    uint64_t end_time;
    uint64_t t0;
    uint64_t t1;
    double   x, y, w, h, maxv;
    uint32_t i0;

    FmonChartPoint *pts[3];
    size_t          n[3];
} CpuAvgCache;

static CpuAvgCache s_avg_cache;

static bool cpu_avg_cache_match(const CpuAvgCache *c, const CurveData *cd,
                                uint32_t count, uint64_t t0, uint64_t t1,
                                double x, double y, double w, double h,
                                double maxv, uint32_t i0)
{
    return c->pts[0] != NULL && c->cd == cd && c->history == cd->history &&
           c->time_us == cd->time_us && c->count == count &&
           c->index == cd->index && c->end_time == cd->end_time_us &&
           c->t0 == t0 && c->t1 == t1 && c->x == x && c->y == y &&
           c->w == w && c->h == h && c->maxv == maxv && c->i0 == i0;
}

static void cpu_avg_cache_invalidate(CpuAvgCache *c)
{
    for (int i = 0; i < 3; i++) {
        g_free(c->pts[i]);
        c->pts[i] = NULL;
        c->n[i] = 0;
    }
}

static void cpu_avg_band_color(int s, gboolean dark, double *r,
                               double *g, double *b)
{
    if (dark) {
        if (s == 0) { *r = 0.0; *g = 1.0; *b = 0.0; }
        else if (s == 1) { *r = 0.0; *g = 1.0; *b = 1.0; }
        else { *r = 1.0; *g = 1.0; *b = 0.0; }
    } else {
        /* 浅色主题：深绿/深蓝/深橙，保证在浅底上可分辨。 */
        if (s == 0) { *r = 0.0; *g = 0.45; *b = 0.0; }
        else if (s == 1) { *r = 0.0; *g = 0.35; *b = 0.80; }
        else { *r = 0.80; *g = 0.40; *b = 0.0; }
    }
}

static void draw_avg_curves(cairo_t *cr, const CurveData *cd, double x,
                            double y, double w, double h, double maxv,
                            int active)
{
    if (cd == NULL || cd->count < 2 || !(maxv > 0.0))
        return;

    uint32_t count = (cd->count > FMON_HISTORY_POINTS) ? FMON_HISTORY_POINTS
                                                       : cd->count;
    uint32_t i0 = (count > 20) ? 15 : 0;

    uint64_t t0 = 0, t1 = 0;
    bool has_time = gui_chart_view_range(cd, count, &t0, &t1);

    if (!has_time)
        (void)fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS, count,
                                    cd->index, &t0, &t1);

    static const struct {
        uint32_t win;
        double   r, g, b;
    } k_bands[] = {
        { 60, 0.00, 1.00, 0.00 },   /* 1 分钟：亮绿 */
        { 300, 0.00, 1.00, 1.00 },  /* 5 分钟：青 */
        { 600, 1.00, 1.00, 0.00 },  /* 10 分钟：黄 */
    };

    CpuAvgCache *cache = &s_avg_cache;
    gboolean dark = fmon_theme_is_dark();

    if (!cpu_avg_cache_match(cache, cd, count, t0, t1, x, y, w, h, maxv,
                             i0)) {
        cpu_avg_cache_invalidate(cache);

        static double   prefix[FMON_HISTORY_POINTS + 1];
        static uint32_t counts[FMON_HISTORY_POINTS + 1];

        prefix[0] = 0.0;
        counts[0] = 0;

        for (uint32_t i = 0; i < count; i++) {
            double v = cpu_curve_hist_at(cd, i);

            prefix[i + 1] = prefix[i] + (isfinite(v) ? v : 0.0);
            counts[i + 1] = counts[i] + (isfinite(v) ? 1u : 0u);
        }

        for (int s = 0; s < 3; s++) {
            uint32_t win = k_bands[s].win;
            size_t cap = (size_t)count + 4;

            cache->pts[s] = g_new0(FmonChartPoint, cap);

            size_t n = 0;
            bool gap = true;

            for (uint32_t i = i0; i < count; i++) {
                uint32_t from = (i + 1 > win) ? i + 1 - win : 0;

                if (from < i0)
                    from = i0;

                uint32_t cnt = counts[i + 1] - counts[from];

                if (cnt == 0) {
                    gap = true;
                    continue;
                }

                double v = (prefix[i + 1] - prefix[from]) / (double)cnt;
                double xx = (count > 1)
                                ? x + w * (double)i / (double)(count - 1)
                                : x;

                cache->pts[s][n].x = xx;
                cache->pts[s][n].y = y + h - (v / maxv) * h;
                cache->pts[s][n].gap = gap;
                cache->pts[s][n].src = i;
                n++;
                gap = false;
            }

            if (n > 0 && cd->time_us != NULL)
                fmon_chart_retime(cache->pts[s], n, cd->time_us,
                                  FMON_HISTORY_POINTS, count, cd->index, x,
                                  w, t0, t1, cd->interval_us * 5u / 2u);

            cache->n[s] = n;
        }

        cache->cd = cd;
        cache->history = cd->history;
        cache->time_us = cd->time_us;
        cache->count = count;
        cache->index = cd->index;
        cache->end_time = cd->end_time_us;
        cache->t0 = t0;
        cache->t1 = t1;
        cache->x = x;
        cache->y = y;
        cache->w = w;
        cache->h = h;
        cache->maxv = maxv;
        cache->i0 = i0;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    for (int s = 0; s < 3; s++) {
        uint32_t win = k_bands[s].win;
        int      on  = (s == active);
        FmonChartPoint *pts = cache->pts[s];
        size_t n = cache->n[s];

        if (n == 0)
            continue;

        cairo_set_line_width(cr, on ? 1.5 : 1.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

        double br, bg, bb;

        cpu_avg_band_color(s, dark, &br, &bg, &bb);
        cairo_set_source_rgb(cr, br, bg, bb);

        fmon_chart_cairo_step_path(cr, pts, n);
        cairo_stroke(cr);

        double ex = pts[n - 1].x;
        double ey = pts[n - 1].y;

        cairo_set_source_rgb(cr, br, bg, bb);
        cairo_arc(cr, ex, ey, on ? 3.0 : 2.0, 0.0, 2.0 * G_PI);
        cairo_fill(cr);

        if (on) {
            double win_val = cpu_curve_avg_window_value(cd, win);

            if (isfinite(win_val)) {
                char vb[32];

                g_snprintf(vb, sizeof(vb), "%.1f%%", win_val);
                gui_cpu_gauge_font(cr, 12.0);

                cairo_text_extents_t ext;

                cairo_text_extents(cr, vb, &ext);
                cairo_move_to(cr, ex - ext.width - 10.0, ey - 10.0);
                cairo_show_text(cr, vb);
            }
        }
    }

    cairo_restore(cr);
}

static void cpu_avg_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                         gpointer data)
{
    (void)data;

    const CurveData *use = gui_metric_curve(0);

    if (use == NULL)
        return;

    GdkRGBA fg;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);

    gui_cpu_gauge_font(cr, 13.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.90);
    cairo_move_to(cr, 12.0, 18.0);
    cairo_show_text(cr, "CPU 平均使用率（1 / 5 / 10 分钟滑动平均）");

    double lx = 48.0, ly = 50.0;
    double lw = w - lx - 16.0;
    double lh = h - ly - 26.0;

    if (lw < 40.0 || lh < 40.0)
        return;

    double amax = cpu_curve_avg_window_max(use, 60);
    double m5   = cpu_curve_avg_window_max(use, 300);
    double m10  = cpu_curve_avg_window_max(use, 600);

    if (m5 > amax)
        amax = m5;
    if (m10 > amax)
        amax = m10;

    double nice = cpu_curve_nice_max(amax);
    int    dec  = (nice / 5.0 < 5.0) ? 1 : 0;
    int    rot  = (int)((g_get_monotonic_time() / G_TIME_SPAN_SECOND / 3) % 3);

    cpu_chart_draw_axis(cr, &fg, lx, ly, lw, lh, nice, 5, dec, "%");
    draw_avg_curves(cr, use, lx, ly, lw, lh, nice, rot);

    static const uint32_t leg_win[3] = { 60, 300, 600 };
    static const char *const leg_txt[3] = { "1 分钟", "5 分钟", "10 分钟" };
    double lgd_x = 12.0;

    gui_cpu_gauge_font(cr, 9.5);

    gboolean dark = fmon_theme_is_dark();

    for (int i = 0; i < 3; i++) {
        double lr, lg, lbb;

        cpu_avg_band_color(i, dark, &lr, &lg, &lbb);
        cairo_set_source_rgb(cr, lr, lg, lbb);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, lgd_x, 35.0);
        cairo_line_to(cr, lgd_x + 14.0, 35.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue,
                              (i == rot) ? 0.95 : 0.55);

        char lb[48];

        double leg_val = cpu_curve_avg_window_value(use, leg_win[i]);

        if (isfinite(leg_val))
            g_snprintf(lb, sizeof(lb), "%s %.1f%%", leg_txt[i], leg_val);
        else
            g_snprintf(lb, sizeof(lb), "%s —", leg_txt[i]);

        cairo_move_to(cr, lgd_x + 18.0, 38.0);
        cairo_show_text(cr, lb);
        lgd_x += 88.0;
    }

    gui_chart_draw_selection(cr, GTK_WIDGET(area), lx, ly, lw, lh);
    cpu_chart_draw_time_axis(cr, use, lx, ly, lw, lh, &fg);
}

GtkWidget *gui_cpu_thermal_card(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_add_css_class(card, "page-card");

    GtkWidget *t = gtk_label_new(_("CPU 负载与温度"));

    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(card), t);

    GtkWidget *s =
        gtk_label_new(
            _("左：CPU 使用率的 1/5/10 分钟平均值（%）· 右：温度曲线 + CPU 温度计"));

    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_box_append(GTK_BOX(card), s);

    /* 两张独立绘图区：各自的框选/缩放只作用于自己的时间轴。
     * 使用普通 Box 而不是 FlowBox，避免 FlowBox 抢走绘图区的滚轮/拖动事件。 */
    GtkWidget *charts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_3);

    s_avg_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_avg_area, 480, 300);
    gtk_widget_set_hexpand(s_avg_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_avg_area),
                                   cpu_avg_draw, NULL, NULL);
    gui_chart_attach_interactions(s_avg_area, gui_metric_curve(0));

    s_temp_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_temp_area, 480, 300);
    gtk_widget_set_hexpand(s_temp_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_temp_area),
                                   gui_cpu_temp_draw, NULL, NULL);
    gui_chart_attach_interactions(s_temp_area, gui_metric_curve(12));

    gtk_box_append(GTK_BOX(charts), s_avg_area);
    gtk_box_append(GTK_BOX(charts), s_temp_area);
    gtk_box_append(GTK_BOX(card), charts);

    return card;
}

void gui_cpu_thermal_update(void)
{
    if (s_avg_area == NULL || s_temp_area == NULL)
        return;

    if (gtk_widget_get_mapped(s_avg_area))
        gtk_widget_queue_draw(s_avg_area);
    if (gtk_widget_get_mapped(s_temp_area))
        gtk_widget_queue_draw(s_temp_area);
}
