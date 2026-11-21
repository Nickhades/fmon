/*
 * chart_geom_cairo.c - Cairo 路径/边界带绘制
 */

#include "chart_geom.h"

#include <cairo.h>

void fmon_chart_cairo_path(cairo_t *cr, const FmonChartPoint *pts, size_t n)
{
    if (cr == NULL || pts == NULL)
        return;

    for (size_t i = 0; i < n; i++) {
        if (i == 0 || pts[i].gap)
            cairo_move_to(cr, pts[i].x, pts[i].y);
        else
            cairo_line_to(cr, pts[i].x, pts[i].y);
    }
}

void fmon_chart_cairo_step_path(cairo_t *cr, const FmonChartPoint *pts,
                                size_t n)
{
    if (cr == NULL || pts == NULL)
        return;

    for (size_t i = 0; i < n; i++) {
        if (i == 0 || pts[i].gap) {
            cairo_move_to(cr, pts[i].x, pts[i].y);
            continue;
        }

        /* 先水平走到新时间列，再垂直落到新采样值：真正的直角阶跃。 */
        cairo_line_to(cr, pts[i].x, pts[i - 1].y);
        cairo_line_to(cr, pts[i].x, pts[i].y);
    }
}

void fmon_chart_cairo_step_fill(cairo_t *cr, const FmonChartPoint *pts,
                                size_t n, double baseline)
{
    if (cr == NULL || pts == NULL || n == 0)
        return;

    size_t i = 0;

    while (i < n) {
        size_t j = i;

        while (j + 1 < n && !pts[j + 1].gap)
            j++;

        cairo_move_to(cr, pts[i].x, baseline);

        for (size_t k = i; k <= j; k++) {
            if (k == i)
                cairo_line_to(cr, pts[k].x, pts[k].y);
            else {
                cairo_line_to(cr, pts[k].x, pts[k - 1].y);
                cairo_line_to(cr, pts[k].x, pts[k].y);
            }
        }

        cairo_line_to(cr, pts[j].x, baseline);
        cairo_close_path(cr);

        i = j + 1;
    }
}

void fmon_chart_cairo_band(cairo_t *cr, const FmonChartPoint *lower,
                           const FmonChartPoint *upper, size_t n)
{
    if (cr == NULL || lower == NULL || upper == NULL || n == 0)
        return;

    size_t i = 0;

    while (i < n) {
        size_t j = i;

        while (j + 1 < n && !lower[j + 1].gap && !upper[j + 1].gap)
            j++;

        cairo_move_to(cr, upper[i].x, upper[i].y);

        for (size_t k = i + 1; k <= j; k++)
            cairo_line_to(cr, upper[k].x, upper[k].y);

        for (size_t k = j + 1; k > i;) {
            k--;
            cairo_line_to(cr, lower[k].x, lower[k].y);
        }

        cairo_close_path(cr);
        i = j + 1;
    }
}

void fmon_chart_cairo_fill(cairo_t *cr, const FmonChartPoint *pts, size_t n,
                           double baseline)
{
    if (cr == NULL || pts == NULL || n == 0)
        return;

    size_t i = 0;

    while (i < n) {
        size_t j = i;

        while (j + 1 < n && !pts[j + 1].gap)
            j++;

        cairo_move_to(cr, pts[i].x, baseline);

        for (size_t k = i; k <= j; k++)
            cairo_line_to(cr, pts[k].x, pts[k].y);

        cairo_line_to(cr, pts[j].x, baseline);
        cairo_close_path(cr);

        i = j + 1;
    }
}
