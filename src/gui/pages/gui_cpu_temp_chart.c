/*
 * gui_cpu_temp_chart.c - CPU 温度曲线与温度计
 */

#include "gui_cpu_temp_chart.h"

#include "chart_geom.h"
#include "cpu_chart_util.h"
#include "cpu_curve_util.h"
#include "gui_chart.h"
#include "gui_chart_util.h"
#include "gui_cpu_gauge.h"
#include "gui_pages.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>

static void draw_thermal_series(cairo_t *cr, const CurveData *cd, double x,
                               double y, double w, double h, double maxv,
                               double r, double g, double b)
{
    if (cd == NULL || cd->count < 2 || !(maxv > 0.0))
        return;

    size_t cap = (size_t)((cd->count > FMON_HISTORY_POINTS)
                              ? FMON_HISTORY_POINTS
                              : cd->count) +
                 4;
    FmonChartPoint *pts = g_new0(FmonChartPoint, cap);

    uint64_t t0 = 0, t1 = 0;
    if (!gui_chart_view_range(cd, cd->count, &t0, &t1))
        fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS, cd->count,
                              cd->index, &t0, &t1);
    uint64_t gap_us = cd->interval_us * 5u / 2u;

    size_t n = fmon_chart_polyline_at(
        cd->history, cd->time_us, FMON_HISTORY_POINTS, cd->count, cd->index,
        t0, t1, x, w, y, h, 0.0, maxv, (uint32_t)w, gap_us, pts, cap);

    if (n == 0) {
        g_free(pts);
        return;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    /* 纯阶梯线：无面积填充、无渐变。 */
    cairo_set_line_width(cr, 1.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_source_rgb(cr, r, g, b);
    fmon_chart_cairo_step_path(cr, pts, n);
    cairo_stroke(cr);

    /* 温度告警阈值线：使用当前 AlertSettings，不参与数据平滑。 */
    double tv;
    char   tlabel[64];

    if (gui_chart_curve_threshold(cd, &tv, tlabel, sizeof(tlabel)) &&
        tv > 0.0 && tv <= maxv) {
        double ty = y + h - (tv / maxv) * h;

        cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.82);
        cairo_set_line_width(cr, 1.0);
        double dash[] = { 5.0, 4.0 };
        cairo_set_dash(cr, dash, 2, 0.0);
        cairo_move_to(cr, x, ty);
        cairo_line_to(cr, x + w, ty);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);

        gui_cpu_gauge_font(cr, 9.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, tlabel, &ext);
        cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.90);
        cairo_move_to(cr, x + 4.0, ty - 3.0);
        cairo_show_text(cr, tlabel);

        for (size_t k = 1; k < n; k++) {
            if (pts[k - 1].gap || pts[k].gap)
                continue;

            double y0 = pts[k - 1].y;
            double y1 = pts[k].y;

            if ((y0 - ty) * (y1 - ty) > 0.0 || y0 == y1)
                continue;

            double tt = (ty - y0) / (y1 - y0);
            double cx = pts[k - 1].x + tt * (pts[k].x - pts[k - 1].x);

            cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.95);
            cairo_move_to(cr, cx, ty - 3.5);
            cairo_line_to(cr, cx - 3.0, ty + 2.0);
            cairo_line_to(cr, cx + 3.0, ty + 2.0);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }

    /* 统计图例：min / avg / max，只统计有限真实采样。 */
    if (cd->count >= 2) {
        double   mn = INFINITY, mx = -INFINITY, sum = 0.0, last = NAN;
        uint32_t valid = 0;

        for (uint32_t i = 0; i < cd->count && i < FMON_HISTORY_POINTS; i++) {
            if (gui_chart_window_us() > 0) {
                uint64_t ti;

                if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS,
                                             cd->count, cd->index, i, &ti))
                    continue;
                if (ti < t0 || ti > t1)
                    continue;
            }

            double v = cpu_curve_hist_at(cd, i);

            if (!isfinite(v))
                continue;
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
            sum += v;
            last = v;
            valid++;
        }

        if (valid > 0) {
            char stat[160];

            g_snprintf(stat, sizeof(stat),
                       "min %.1f°C · avg %.1f°C · max %.1f°C · last %.1f°C",
                       mn, sum / (double)valid, mx, last);
            gui_cpu_gauge_font(cr, 9.0);
            cairo_set_source_rgba(cr, 0.92, 0.30, 0.25, 0.60);
            cairo_move_to(cr, x + 6.0, y + h - 6.0);
            cairo_show_text(cr, stat);
        }
    }

    cairo_restore(cr);
    g_free(pts);
}

/* 1/5/10 分钟滑动平均，三条线性曲线；用前缀和保证 O(n)。
 * 平均窗口内没有有效采样时断开折线，不把缺口当成 0。 */
static void draw_thermometer(cairo_t *cr, const GdkRGBA *fg, double x,
                             double top, double h, double temp)
{
    double bw = 14.0;

    if (!isfinite(temp))
        temp = 0.0;

    double frac = temp / 100.0;

    if (frac < 0.0)
        frac = 0.0;
    if (frac > 1.0)
        frac = 1.0;

    /* 轨道 */
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.10);
    cairo_rectangle(cr, x, top, bw, h);
    cairo_fill(cr);

    /* 填充：按温度分级着色（绿 / 黄 / 红） */
    double fy;

    if (temp >= 85.0)
        cairo_set_source_rgba(cr, 0.88, 0.16, 0.14, 0.92);
    else if (temp >= 70.0)
        cairo_set_source_rgba(cr, 0.92, 0.65, 0.06, 0.92);
    else
        cairo_set_source_rgba(cr, 0.20, 0.78, 0.45, 0.92);

    fy = top + h * (1.0 - frac);
    cairo_rectangle(cr, x, fy, bw, h - (fy - top));
    cairo_fill(cr);

    /* 刻度 */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.30);

    for (int k = 0; k <= 4; k++) {
        double yy = top + h * (double)k / 4.0;

        cairo_move_to(cr, x + bw + 2.0, yy);
        cairo_line_to(cr, x + bw + 6.0, yy);
    }

    cairo_stroke(cr);
}


void gui_cpu_temp_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                          gpointer data)
{
    (void)data;

    const CurveData *temp = gui_metric_curve(12);

    if (temp == NULL)
        return;

    GdkRGBA fg;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);

    gui_cpu_gauge_font(cr, 13.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.90);
    cairo_move_to(cr, 12.0, 18.0);
    cairo_show_text(cr, "CPU 温度");

    double ly = 50.0;
    double lh = h - ly - 26.0;

    if (lh < 40.0)
        return;

    uint32_t temp_n = (temp->count > FMON_HISTORY_POINTS)
                          ? FMON_HISTORY_POINTS
                          : temp->count;
    double cur_temp = (temp_n > 0) ? cpu_curve_hist_at(temp, temp_n - 1) : NAN;

    draw_thermometer(cr, &fg, 20.0, ly + 6.0, lh - 22.0, cur_temp);

    double tx = 96.0;
    double twd = w - tx - 16.0;

    if (twd < 40.0)
        return;

    double tmax = 100.0;
    double tlo = 0.0, thi = 0.0;

    if (fmon_chart_history_range(temp->history, FMON_HISTORY_POINTS,
                                 temp->count, temp->index, &tlo, &thi) &&
        thi > tmax)
        tmax = ceil(thi / 20.0) * 20.0;

    cpu_chart_draw_axis(cr, &fg, tx, ly, twd, lh, tmax, 4, 0, "°C");
    draw_thermal_series(cr, temp, tx, ly, twd, lh, tmax, 0.92, 0.30, 0.25);

    char b[32];

    gui_cpu_gauge_font(cr, 26.0);

    if (isfinite(cur_temp))
        g_snprintf(b, sizeof(b), "%.1f°C", cur_temp);
    else
        g_snprintf(b, sizeof(b), "—");

    cairo_text_extents_t ext;

    cairo_text_extents(cr, b, &ext);

    if (isfinite(cur_temp) && cur_temp >= 85.0)
        cairo_set_source_rgba(cr, 0.88, 0.16, 0.14, 0.95);
    else if (isfinite(cur_temp) && cur_temp >= 70.0)
        cairo_set_source_rgba(cr, 0.92, 0.65, 0.06, 0.95);
    else if (isfinite(cur_temp))
        cairo_set_source_rgba(cr, 0.92, 0.30, 0.25, 0.95);
    else
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.55);

    cairo_move_to(cr, tx + twd - ext.width - 8.0, ly + 26.0);
    cairo_show_text(cr, b);

    gui_chart_draw_selection(cr, GTK_WIDGET(area), tx, ly, twd, lh);
    cpu_chart_draw_time_axis(cr, temp, tx, ly, twd, lh, &fg);
}
