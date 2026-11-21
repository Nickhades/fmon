/*
 * cpu_chart_util.c - CPU 曲线共用坐标轴
 */

#include "cpu_chart_util.h"

#include "cpu_chart_util.h"

#include "chart_geom.h"
#include "gui_cpu_gauge.h"
#include "gui_chart_util.h"

#include <math.h>

void cpu_chart_draw_axis(cairo_t *cr, const GdkRGBA *fg, double x, double y,
                      double w, double h, double maxv, int ticks,
                      int decimals, const char *suffix)
{
    for (int k = 0; k <= ticks; k++) {
        double v  = maxv * (double)k / (double)ticks;
        double yy = y + h - (v / maxv) * h;

        yy = floor(yy) + 0.5;

        cairo_set_line_width(cr, 0.8);
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.10);
        cairo_move_to(cr, x, yy);
        cairo_line_to(cr, x + w, yy);
        cairo_stroke(cr);

        char b[32];

        g_snprintf(b, sizeof(b), "%.*f%s", decimals, v, suffix);
        gui_cpu_gauge_font(cr, 9.5);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, b, &ext);
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.55);
        cairo_move_to(cr, x - ext.width - 6.0, yy + 3.5);
        cairo_show_text(cr, b);
    }
}

void cpu_chart_draw_time_axis(cairo_t *cr, const CurveData *cd,
                               double x, double y, double w, double h,
                               const GdkRGBA *fg)
{
    if (cd == NULL || cd->count < 2 || w < 120.0)
        return;

    uint32_t n = MIN(cd->count, FMON_HISTORY_POINTS);
    uint64_t t0 = 0, t1 = 0;
    uint64_t data_t0 = 0, data_t1 = 0;

    if (!gui_chart_view_range(cd, n, &t0, &t1) || t1 <= t0)
        return;

    if (!fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS, n, cd->index,
                               &data_t0, &data_t1))
        return;

    double span_sec = (double)(t1 - t0) / 1000000.0;
    double offset = (data_t1 > t1) ? (double)(data_t1 - t1) / 1000000.0 : 0.0;

    gui_cpu_gauge_font(cr, 9.5);

    for (int i = 0; i <= 3; i++) {
        double frac = (double)i / 3.0;
        double secs = span_sec * (1.0 - frac) + offset;
        char   buf[32];

        gui_chart_format_time_label(buf, sizeof(buf), secs);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, buf, &ext);

        double tx = x + frac * w - ext.width / 2.0;

        if (i == 0)
            tx = x;
        if (i == 3)
            tx = x + w - ext.width;

        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.60);
        cairo_move_to(cr, tx, y + h + 14.0);
        cairo_show_text(cr, buf);
    }
}
