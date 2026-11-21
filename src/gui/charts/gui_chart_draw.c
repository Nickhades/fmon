/*
 * gui_chart_draw.c - 图表坐标、阈值、统计与边框
 */

#include "gui_chart.h"
#include "gui_chart_draw.h"

#include "chart_geom.h"
#include "gui_chart_util.h"
#include "gui_util.h"
#include "theme.h"
#include "alerts.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

gboolean gui_chart_auto_range_for(const CurveData *cd)
{
    return gui_chart_auto_range() && cd != NULL && cd->unit != NULL &&
           strcmp(cd->unit, "%") == 0;
}

void gui_chart_set_font(cairo_t *cr)
{
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
}

gboolean gui_chart_curve_threshold(const CurveData *cd, double *value,
                                   char *label, size_t label_len)
{
    if (cd == NULL || cd->alert_settings == NULL || !cd->alert_settings->enabled ||
        value == NULL || label == NULL || label_len == 0)
        return FALSE;

    const AlertSettings *a = cd->alert_settings;
    double v = 0.0;
    const char *name = NULL;

    switch (cd->alert_metric) {
    case 0:
        v = a->cpu_pct;
        name = "CPU";
        break;
    case 1:
        v = a->mem_pct;
        name = "内存";
        break;
    case 2:
        v = a->swap_pct;
        name = "Swap";
        break;
    case 6:
        v = a->pool_pct;
        name = "池";
        break;
    case 12:
        v = a->temp_c;
        name = "温度";
        break;
    case 13:
        v = a->battery_pct;
        name = "低电量";
        break;
    default:
        return FALSE;
    }

    if (!isfinite(v) || v <= 0.0)
        return FALSE;

    *value = v;

    if (cd->unit != NULL && strcmp(cd->unit, "%") == 0)
        g_snprintf(label, label_len, "%s %.0f%%", name, v);
    else if (cd->unit != NULL && strcmp(cd->unit, "°C") == 0)
        g_snprintf(label, label_len, "%s %.0f°C", name, v);
    else
        g_snprintf(label, label_len, "%s %.1f%s", name, v,
                   (cd->unit != NULL) ? cd->unit : "");

    return TRUE;
}

void gui_chart_draw_hint(cairo_t *cr, int left, int top, int plot_w, int plot_h,
                      const char *text)
{
    if (text == NULL)
        return;

    cairo_text_extents_t ext;

    cairo_text_extents(cr, text, &ext);

    cairo_set_source_rgba(cr, 0.60, 0.60, 0.65, 0.85);
    cairo_move_to(cr, left + (plot_w - ext.width) / 2.0,
                  top + (plot_h + ext.height) / 2.0);
    cairo_show_text(cr, text);
}

/* 窗口内所有有效数据（平均线 + 可选 min/max 包络）的范围。
 * time_window 为 TRUE 时只统计 [t0, t1] 内的真实采样。 */
bool gui_chart_curve_range(const CurveData *cd, uint32_t points, uint64_t t0,
                        uint64_t t1, bool time_window, double *lo,
                        double *hi)
{
    const double *series[3] = { cd->history, cd->history_min,
                                cd->history_max };
    bool          have = false;
    double        l = 0.0, h = 0.0;

    for (int s = 0; s < 3; s++) {
        if (series[s] == NULL)
            continue;

        for (uint32_t i = 0; i < points && i < FMON_HISTORY_POINTS; i++) {
            if (time_window) {
                uint64_t ti;

                if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS,
                                             points, cd->index, i, &ti))
                    continue;
                if (ti < t0 || ti > t1)
                    continue;
            }

            double v;

            if (!fmon_chart_history_value(series[s], FMON_HISTORY_POINTS,
                                          points, cd->index, i, &v))
                continue;
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
    }

    if (have) {
        *lo = l;
        *hi = h;
    }

    return have;
}

uint64_t gui_chart_gap_threshold_us(const CurveData *cd)
{
    if (cd->interval_us == 0)
        return 0;

    /* 2.5 个标称间隔仍未采样，就认为发生了真实时间缺口。 */
    return cd->interval_us * 5u / 2u;
}

bool gui_chart_nearest_sample_by_time(const CurveData *cd, uint32_t points,
                                   uint64_t t, uint32_t *out_idx)
{
    uint64_t best = UINT64_MAX;
    uint32_t best_i = 0;

    for (uint32_t i = 0; i < points; i++) {
        uint64_t ti;
        double   v;

        if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS, points,
                                     cd->index, i, &ti))
            continue;
        if (!fmon_chart_history_value(cd->history, FMON_HISTORY_POINTS, points,
                                      cd->index, i, &v))
            continue;

        uint64_t d = (ti > t) ? ti - t : t - ti;

        if (d < best) {
            best   = d;
            best_i = i;
        }
    }

    if (best == UINT64_MAX)
        return false;

    *out_idx = best_i;
    return true;
}

void gui_chart_draw_threshold_markers(cairo_t *cr, const CurveData *cd,
                                     const FmonChartScale *sc,
                                     const FmonChartPoint *pts, size_t n,
                                     int margin_top, int plot_h)
{
    double value;
    char   label[64];

    if (!gui_chart_curve_threshold(cd, &value, label, sizeof(label)))
        return;
    if (sc == NULL || sc->hi <= sc->lo || value < sc->lo || value > sc->hi)
        return;

    double ty = margin_top + (double)plot_h * (sc->hi - value) /
                                  (sc->hi - sc->lo);

    for (size_t i = 1; i < n; i++) {
        if (pts[i - 1].gap || pts[i].gap)
            continue;

        double y0 = pts[i - 1].y;
        double y1 = pts[i].y;

        if ((y0 - ty) * (y1 - ty) > 0.0 || y0 == y1)
            continue;

        double t = (ty - y0) / (y1 - y0);
        double x = pts[i - 1].x + t * (pts[i].x - pts[i - 1].x);

        cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.95);
        cairo_move_to(cr, x, ty - 3.5);
        cairo_line_to(cr, x - 3.0, ty + 2.0);
        cairo_line_to(cr, x + 3.0, ty + 2.0);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

static double snap_1px(double v)
{
    /* 1px 网格/坐标轴对齐像素中心；数据折线不做 snap。 */
    return floor(v) + 0.5;
}

void gui_chart_draw_threshold(cairo_t *cr, const CurveData *cd,
                           const FmonChartScale *sc, int margin_left,
                           int margin_top, int plot_w, int plot_h)
{
    double value;
    char   label[64];

    if (!gui_chart_curve_threshold(cd, &value, label, sizeof(label)))
        return;
    if (sc == NULL || sc->hi <= sc->lo || value < sc->lo || value > sc->hi)
        return;

    double y = snap_1px(margin_top + (double)plot_h *
                                       (sc->hi - value) / (sc->hi - sc->lo));

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.82);
    cairo_set_line_width(cr, 1.0);
    double dash[] = { 5.0, 4.0 };
    cairo_set_dash(cr, dash, 2, 0.0);
    cairo_move_to(cr, margin_left, y);
    cairo_line_to(cr, margin_left + plot_w, y);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);

    gui_chart_set_font(cr);
    cairo_set_font_size(cr, 9.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, label, &ext);
    double lx = margin_left + plot_w - ext.width - 4.0;
    double ly = (y - 3.0 > margin_top + 10.0) ? y - 3.0
                                               : y + ext.height + 3.0;
    cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.90);
    cairo_move_to(cr, lx, ly);
    cairo_show_text(cr, label);
    gui_chart_set_font(cr);
    cairo_restore(cr);
}

void gui_chart_draw_stats(cairo_t *cr, const CurveData *cd,
                        uint32_t points, uint64_t t0, uint64_t t1,
                        bool time_window, int margin_left, int margin_top,
                        int plot_w)
{
    if (points < 2 || cd->history == NULL)
        return;

    double   mn = INFINITY, mx = -INFINITY, sum = 0.0;
    double   last = NAN;
    uint32_t n = 0;

    for (uint32_t i = 0; i < points; i++) {
        double v;

        if (time_window) {
            uint64_t ti;

            if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS,
                                         points, cd->index, i, &ti))
                continue;
            if (ti < t0 || ti > t1)
                continue;
        }

        if (!fmon_chart_history_value(cd->history, FMON_HISTORY_POINTS,
                                      points, cd->index, i, &v))
            continue;
        if (!isfinite(v))
            continue;

        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
        sum += v;
        last = v;
        n++;
    }

    if (n == 0)
        return;

    char mn_txt[32], av_txt[32], mx_txt[32], last_txt[32], text[220];

    gui_chart_format_value(mn_txt, sizeof(mn_txt), mn, cd->unit);
    gui_chart_format_value(av_txt, sizeof(av_txt), sum / (double)n, cd->unit);
    gui_chart_format_value(mx_txt, sizeof(mx_txt), mx, cd->unit);
    gui_chart_format_value(last_txt, sizeof(last_txt), last, cd->unit);

    g_snprintf(text, sizeof(text), "min %s · avg %s · max %s · last %s",
               mn_txt, av_txt, mx_txt, last_txt);

    gui_chart_set_font(cr);
    cairo_set_font_size(cr, 9.0);

    cairo_text_extents_t ext;

    cairo_text_extents(cr, text, &ext);

    if (ext.width + 10.0 > plot_w - 8.0)
        return;

    double x = margin_left + 4.0;
    double y = margin_top + 4.0;
    double box_h = ext.height + 4.0;

    double bg[4];

    fmon_theme_hover_bg(bg);
    cairo_set_source_rgba(cr, bg[0], bg[1], bg[2], 0.72);
    cairo_rectangle(cr, x - 2.0, y, ext.width + 4.0, box_h);
    cairo_fill(cr);

    double axis[4];

    fmon_theme_axis_color(axis);
    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.78);
    cairo_move_to(cr, x, y + ext.height);
    cairo_show_text(cr, text);
    gui_chart_set_font(cr);
}

/* 画一次背景/网格/坐标轴。 */
void gui_chart_draw_frame(cairo_t *cr, const CurveData *cd,
                       const FmonChartScale *sc, int margin_left,
                       int margin_top, int plot_w, int plot_h)
{
    double axis_col[4];

    fmon_theme_axis_color(axis_col);

    /* 曲线区域独立面板背景（与窗口背景区分） */
    fmon_gui_draw_panel(cr, margin_left - 6.0, margin_top - 6.0,
                        (double)plot_w + 14.0, (double)plot_h + 10.0, 0.60,
                        0.60, 0.65, 0.10);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_width(cr, 1.0);

    for (int i = 0; i <= sc->intervals; i++) {
        double v   = sc->lo + sc->step * (double)i;
        double pos = (sc->hi > sc->lo)
                         ? margin_top + (double)plot_h *
                                            (sc->hi - v) / (sc->hi - sc->lo)
                         : margin_top + (double)plot_h;
        pos = snap_1px(pos);

        double grid[4];

        fmon_theme_grid_color(grid, FALSE);
        cairo_set_source_rgba(cr, grid[0], grid[1], grid[2], grid[3]);
        cairo_move_to(cr, margin_left, pos);
        cairo_line_to(cr, margin_left + plot_w, pos);
        cairo_stroke(cr);

        char label[48];

        gui_chart_format_axis_label(label, sizeof(label), v, cd->unit, sc->step);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, label, &ext);
        cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2], 1.0);
        cairo_move_to(cr, margin_left - ext.width - 6.0,
                      pos + ext.height / 2.0);
        cairo_show_text(cr, label);
    }
}
