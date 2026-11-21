/*
 * combo_chart_geom.c - 组合图表几何与数值格式化
 */

#include "combo_chart_internal.h"

#include "gui_chart_util.h"
#include "gui_util.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double series_value(const CurveData *cd, uint32_t i)
{
    double v;

    if (cd == NULL || cd->history == NULL)
        return NAN;

    if (!fmon_chart_history_value(cd->history, COMBO_MAX_POINTS, cd->count,
                                  cd->index, i, &v))
        return NAN;

    return v;
}

bool series_range(const CurveData *cd, double *lo, double *hi)
{
    if (cd == NULL || cd->history == NULL)
        return false;

    uint32_t n = (cd->count > COMBO_MAX_POINTS) ? COMBO_MAX_POINTS
                                                : cd->count;
    uint64_t t0 = 0, t1 = 0;
    bool window = gui_chart_view_range(cd, n, &t0, &t1) &&
                  gui_chart_window_us() > 0;
    bool have = false;
    double l = 0.0, h = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        if (window) {
            uint64_t ti;

            if (!fmon_chart_history_time(cd->time_us, COMBO_MAX_POINTS, n,
                                         cd->index, i, &ti))
                continue;
            if (ti < t0 || ti > t1)
                continue;
        }

        double v = series_value(cd, i);

        if (!isfinite(v))
            continue;

        if (!have) {
            l = h = v;
            have = true;
        } else {
            if (v < l)
                l = v;
            if (v > h)
                h = v;
        }
    }

    if (have) {
        *lo = l;
        *hi = h;
    }

    return have;
}

bool series_scale(const CurveData *cd, FmonChartScale *sc)
{
    double lo = 0.0;
    double hi = 0.0;
    bool   have = series_range(cd, &lo, &hi);

    /* 百分比指标在窄幅波动时至少给 1 个百分点的可视跨度。
     * 这只是把纵轴放大，不改采样值；轴标签会明确显示真实范围。 */
    gboolean pct = (cd != NULL && cd->unit != NULL &&
                    strcmp(cd->unit, "%") == 0);
    double min_span = pct ? 1.0 : 0.0;
    gboolean auto_pct = pct && gui_chart_auto_range();

    return fmon_chart_scale(have ? lo : 0.0, have ? hi : 0.0,
                            auto_pct
                                ? -1.0
                                : ((cd != NULL && cd->min_val >= 0.0)
                                       ? cd->min_val
                                       : -1.0),
                            auto_pct
                                ? 0.0
                                : ((cd != NULL && cd->max_val > 0.0)
                                       ? cd->max_val
                                       : 0.0),
                            min_span, 2, sc);
}

void series_text_color(const CurveData *cd, double *r, double *g, double *b)
{
    *r = cd->r;
    *g = cd->g;
    *b = cd->b;

    if (!fmon_theme_is_dark()) {
        *r *= 0.72;
        *g *= 0.72;
        *b *= 0.72;
    }
}

void format_value_text(char *buf, size_t n, double v, const char *unit)
{
    gui_chart_format_value(buf, n, v, unit);
}

void combo_geom_init(ComboGeom *g, int width, int height)
{
    g->left   = 74.0;
    g->right  = 14.0;
    g->top    = 12.0;
    g->bottom = 26.0;
    g->plot_w = (double)width - g->left - g->right;
    g->plot_h = (double)height - g->top - g->bottom;
}

void band_layout(const ComboGeom *g, int series_count, int s, BandGeom *b)
{
    double band_h = g->plot_h / (double)series_count;

    b->top      = g->top + band_h * (double)s;
    b->bottom   = b->top + band_h;
    b->plot_top = b->top + 26.0;
    b->plot_h   = b->bottom - 8.0 - b->plot_top;

    if (b->plot_h < 10.0)
        b->plot_h = 10.0;
}

double band_y(const BandGeom *b, double v, double lo, double hi)
{
    if (!(hi > lo))
        return b->plot_top + b->plot_h;
    if (!isfinite(v))
        return b->plot_top + b->plot_h;

    return b->plot_top + b->plot_h -
           ((v - lo) / (hi - lo)) * b->plot_h;
}

void set_font(cairo_t *cr, double size)
{
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, size);
}
