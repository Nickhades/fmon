/*
 * combo_chart_geom.c - 组合图表几何与数值格式化
 */

#include "combo_chart_internal.h"

#include "gui_util.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double series_value(const CurveData *cd, uint32_t i)
{
    uint32_t start = (cd->index + COMBO_MAX_POINTS - cd->count) %
                     COMBO_MAX_POINTS;

    return cd->history[(start + i) % COMBO_MAX_POINTS];
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

void series_range(const CurveData *cd, double *lo, double *hi)
{
    double mn = 1e18;
    double mx = -1e18;

    for (uint32_t i = 0; i < cd->count; i++) {
        double v = series_value(cd, i);

        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
    }

    /* 柱形（CPU 使用率）：单个 100% 尖峰会把纵轴拉高、柱子压扁，
     * 用 99.5% 分位作为上限，个别尖峰允许被裁掉。 */
    if (cd->bars && cd->count >= 50) {
        static double tmp[FMON_HISTORY_POINTS];

        for (uint32_t i = 0; i < cd->count; i++)
            tmp[i] = series_value(cd, i);

        qsort(tmp, cd->count, sizeof(double), cmp_double);
        mn = tmp[0];
        mx = tmp[(size_t)((double)cd->count * 0.995)];
    }

    if (cd->count == 0) {
        mn = 0.0;
        mx = 1.0;
    }

    double span = mx - mn;

    /* 百分比指标变化极小时，别把 0.02% 的抖动放大成满屏台阶 */
    if (cd->unit != NULL && strcmp(cd->unit, "%") == 0 && span < 1.0) {
        double mid = (mn + mx) / 2.0;

        mn   = mid - 0.5;
        mx   = mid + 0.5;
        span = 1.0;
    }

    if (span < 1e-9) {
        span = (mx != 0.0) ? fabs(mx) * 0.05 : 1.0;
        if (span < 1e-6)
            span = 1.0;
    }

    *lo = mn - span * 0.12;
    *hi = mx + span * 0.12;

    /* 非负指标（使用率、温度等）不要把刻度拉到负数 */
    if (mn >= 0.0 && *lo < 0.0)
        *lo = 0.0;

    if (*hi - *lo < 1e-9)
        *hi = *lo + 1.0;
}

void series_text_color(const CurveData *cd, double *r, double *g,
                              double *b)
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
    if (unit != NULL && strcmp(unit, "KB/s") == 0) {
        char *txt = fmon_gui_format_rate(v);

        g_snprintf(buf, n, "%s", txt);
        g_free(txt);
        return;
    }

    if (unit != NULL && strcmp(unit, "°C") == 0) {
        g_snprintf(buf, n, "%.1f°C", v);
        return;
    }

    if (unit != NULL && strcmp(unit, "%") == 0) {
        g_snprintf(buf, n, "%.1f%%", v);
        return;
    }

    /* 计数值（如 Prune 回收次数）不显示小数 */
    if (fabs(v - (double)(long long)v) < 1e-9) {
        g_snprintf(buf, n, "%.0f%s", v, (unit != NULL) ? unit : "");
        return;
    }

    g_snprintf(buf, n, "%.2f%s", v, (unit != NULL) ? unit : "");
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

void band_layout(const ComboGeom *g, int series_count, int s,
                        BandGeom *b)
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
    double y = b->plot_top + b->plot_h - ((v - lo) / (hi - lo)) * b->plot_h;

    if (y < b->plot_top)
        y = b->plot_top;
    if (y > b->plot_top + b->plot_h)
        y = b->plot_top + b->plot_h;

    return y;
}

void set_font(cairo_t *cr, double size)
{
    cairo_select_font_face(cr, "Noto Sans CJK SC", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, size);
}

void format_span(char *buf, size_t n, int64_t secs)
{
    if (secs >= 60)
        g_snprintf(buf, n, "-%ld.%ldm", (long)(secs / 60),
                   (long)((secs % 60) / 6));
    else
        g_snprintf(buf, n, "-%lds", (long)secs);
}



