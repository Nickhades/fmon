/*
 * combo_chart_draw.c - 组合图表绘制（网格/带框/曲线/悬停）
 *
 * 每条带都是一条独立的线性折线：真实采样点之间直线连接，缺采样处断开。
 * 纵轴默认按该带数据自动量程；百分比指标使用 0-100 的固定基准，
 * 数据超出时轴会扩展而不是把峰值裁掉。
 */

#include "combo_chart_internal.h"

#include "gui_chart_util.h"
#include "gui_util.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void draw_grid(cairo_t *cr, const ComboGeom *g, int series_count)
{
    double pw = g->plot_w;
    double ph = g->plot_h;

    cairo_set_line_width(cr, 1.0);

    double line[4];

    for (int i = 0; i < 4; i++) {
        double x = floor(g->left + pw * (double)i / 3.0) + 0.5;

        fmon_theme_grid_color(line, FALSE);
        cairo_set_source_rgba(cr, line[0], line[1], line[2], line[3]);
        cairo_move_to(cr, x, g->top);
        cairo_line_to(cr, x, g->top + ph);
        cairo_stroke(cr);
    }

    for (int s = 1; s < series_count; s++) {
        double y = floor(g->top + ph * (double)s /
                         (double)series_count) + 0.5;

        fmon_theme_grid_color(line, TRUE);
        cairo_set_source_rgba(cr, line[0], line[1], line[2], line[3]);
        cairo_move_to(cr, g->left, y);
        cairo_line_to(cr, g->left + pw, y);
        cairo_stroke(cr);
    }
}

void draw_axis_labels(cairo_t *cr, const ComboGeom *g,
                      const ComboChart *c)
{
    if (c->n <= 0)
        return;

    const CurveData *cd0 = c->series[0].cd;
    uint32_t n = (cd0->count > COMBO_MAX_POINTS) ? COMBO_MAX_POINTS
                                                 : cd0->count;

    if (n < 2)
        return;

    uint64_t t0 = 0, t1 = 0;
    bool has_time = gui_chart_view_range(cd0, n, &t0, &t1);
    double span_sec = has_time
                          ? (double)(t1 - t0) / 1000000.0
                          : (double)(n - 1) * (double)cd0->interval_us /
                                1000000.0;

    uint64_t data_t0 = 0, data_t1 = 0;
    double offset = 0.0;

    if (has_time &&
        fmon_chart_time_range(cd0->time_us, COMBO_MAX_POINTS, n, cd0->index,
                              &data_t0, &data_t1) &&
        data_t1 > t1)
        offset = (double)(data_t1 - t1) / 1000000.0;

    set_font(cr, 10.0);

    for (int i = 0; i < 4; i++) {
        double x = g->left + g->plot_w * (double)i / 3.0;
        char   buf[32];

        gui_chart_format_time_label(
            buf, sizeof(buf),
            span_sec * (1.0 - (double)i / 3.0) + offset);

        double axis[4];

        fmon_theme_axis_color(axis);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], axis[3]);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, buf, &ext);

        double tx = x - ext.width / 2.0;

        if (i == 0)
            tx = g->left;
        if (i == 3)
            tx = g->left + g->plot_w - ext.width;

        cairo_move_to(cr, tx, g->top + g->plot_h + 16.0);
        cairo_show_text(cr, buf);
    }
}

void draw_band_frame(cairo_t *cr, const ComboGeom *g,
                     const BandGeom *b, const CurveData *cd)
{
    /* 所有分带使用统一的网格/边框颜色，避免不同系列色把背景割裂 */
    (void)cd;

    double line[4];

    fmon_theme_grid_color(line, TRUE);
    fmon_gui_draw_panel(cr, g->left - 6.0, b->top + 1.0, g->plot_w + 12.0,
                        (b->bottom - b->top) - 4.0, line[0], line[1], line[2],
                        line[3]);
}

void draw_band_label(cairo_t *cr, const ComboGeom *g,
                     const BandGeom *b, const ComboSeries *series,
                     const CurveData *cd)
{
    char label[128];
    char cur_txt[64];

    uint32_t n = (cd->count > COMBO_MAX_POINTS) ? COMBO_MAX_POINTS
                                                : cd->count;
    double cur = (n > 0) ? series_value(cd, n - 1) : NAN;

    format_value_text(cur_txt, sizeof(cur_txt), cur, cd->unit);
    g_snprintf(label, sizeof(label), "%s  %s", series->name, cur_txt);

    double tr, tg, tb;

    series_text_color(cd, &tr, &tg, &tb);

    set_font(cr, 14.0);
    cairo_set_source_rgba(cr, tr, tg, tb, 0.95);
    cairo_move_to(cr, g->left + 8.0, b->top + 20.0);
    cairo_show_text(cr, label);

    /* 第二行：可选统计信息（透明背景，直接画在带内） */
    if (series->extra[0] != '\0') {
        set_font(cr, 11.5);
        cairo_set_source_rgba(cr, tr, tg, tb, 0.72);
        cairo_move_to(cr, g->left + 8.0, b->top + 38.0);
        cairo_show_text(cr, series->extra);
    }

    /* 第三行：窗口内 min / avg / max；与曲线使用同一批原始采样。 */
    if (n >= 2) {
        uint64_t t0 = 0, t1 = 0;
        bool window = gui_chart_view_range(cd, n, &t0, &t1) &&
                      gui_chart_window_us() > 0;
        double mn = INFINITY, mx = -INFINITY, sum = 0.0, last = NAN;
        uint32_t valid = 0;

        for (uint32_t i = 0; i < n; i++) {
            if (window) {
                uint64_t ti;

                if (!fmon_chart_history_time(cd->time_us, COMBO_MAX_POINTS,
                                             n, cd->index, i, &ti))
                    continue;
                if (ti < t0 || ti > t1)
                    continue;
            }

            double v = series_value(cd, i);

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
            char mn_txt[32], av_txt[32], mx_txt[32], last_txt[32];
            char line[220];

            format_value_text(mn_txt, sizeof(mn_txt), mn, cd->unit);
            format_value_text(av_txt, sizeof(av_txt), sum / (double)valid,
                              cd->unit);
            format_value_text(mx_txt, sizeof(mx_txt), mx, cd->unit);
            format_value_text(last_txt, sizeof(last_txt), last, cd->unit);
            g_snprintf(line, sizeof(line),
                       "min %s · avg %s · max %s · last %s", mn_txt, av_txt,
                       mx_txt, last_txt);

            set_font(cr, 10.0);
            cairo_set_source_rgba(cr, tr, tg, tb, 0.62);
            cairo_move_to(cr, g->left + 8.0,
                          b->top + (series->extra[0] != '\0' ? 54.0 : 38.0));
            cairo_show_text(cr, line);
        }
    }
}

void draw_band_ticks(cairo_t *cr, const ComboGeom *g, const BandGeom *b,
                     const FmonChartScale *sc, const char *unit)
{
    if (sc == NULL || !isfinite(sc->lo) || !isfinite(sc->hi))
        return;

    double axis[4];

    fmon_theme_axis_color(axis);
    set_font(cr, 9.5);

    int idx[3] = { 0, sc->intervals / 2, sc->intervals };

    for (int k = 0; k < 3; k++) {
        if (k > 0 && idx[k] == idx[k - 1])
            continue;

        double v = sc->lo + sc->step * (double)idx[k];
        char   txt[48];

        gui_chart_format_axis_label(txt, sizeof(txt), v, unit, sc->step);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, txt, &ext);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.85);

        double tx = g->left - 8.0 - ext.width;

        if (tx < 2.0)
            tx = 2.0;

        cairo_move_to(cr, tx, band_y(b, v, sc->lo, sc->hi) + 3.5);
        cairo_show_text(cr, txt);
    }
}

static double combo_snap_1px(double v)
{
    return floor(v) + 0.5;
}

static void draw_combo_threshold_markers(cairo_t *cr, const ComboGeom *g,
                                           const BandGeom *b,
                                           const CurveData *cd,
                                           const FmonChartScale *sc,
                                           const FmonChartPoint *pts,
                                           size_t n)
{
    double value;
    char   label[64];

    if (!gui_chart_curve_threshold(cd, &value, label, sizeof(label)))
        return;
    if (sc == NULL || sc->hi <= sc->lo || value < sc->lo || value > sc->hi)
        return;

    double ty = band_y(b, value, sc->lo, sc->hi);

    (void)g;

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

static void draw_combo_threshold(cairo_t *cr, const ComboGeom *g,
                                 const BandGeom *b, const CurveData *cd,
                                 const FmonChartScale *sc)
{
    double value;
    char   label[64];

    if (!gui_chart_curve_threshold(cd, &value, label, sizeof(label)))
        return;
    if (sc == NULL || sc->hi <= sc->lo || value < sc->lo || value > sc->hi)
        return;

    double y = combo_snap_1px(band_y(b, value, sc->lo, sc->hi));

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.80);
    cairo_set_line_width(cr, 1.0);
    double dash[] = { 5.0, 4.0 };
    cairo_set_dash(cr, dash, 2, 0.0);
    cairo_move_to(cr, g->left, y);
    cairo_line_to(cr, g->left + g->plot_w, y);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);

    set_font(cr, 9.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, label, &ext);
    double lx = g->left + g->plot_w - ext.width - 8.0;
    double ly = (y - 3.0 > b->plot_top + 10.0) ? y - 3.0
                                                : y + ext.height + 3.0;
    cairo_set_source_rgba(cr, 0.86, 0.18, 0.16, 0.90);
    cairo_move_to(cr, lx, ly);
    cairo_show_text(cr, label);
    cairo_restore(cr);
}

void draw_series_line(cairo_t *cr, const ComboGeom *g,
                      const BandGeom *b, const CurveData *cd,
                      const FmonChartScale *sc, const FmonChartPoint *pts,
                      size_t n)
{
    if (cd->count < 2 || sc == NULL)
        return;

    if (cd->bars) {
        double bottom = b->plot_top + b->plot_h;

        int nb = (int)(g->plot_w / 9.0);

        if (nb < 12)
            nb = 12;
        if ((uint32_t)nb > cd->count)
            nb = (int)cd->count;

        double slot = g->plot_w / (double)nb;

        for (int k = 0; k < nb; k++) {
            uint32_t s0 = (uint32_t)((uint64_t)cd->count * (uint32_t)k /
                                     (uint32_t)nb);
            uint32_t s1 = (uint32_t)((uint64_t)cd->count * (uint32_t)(k + 1) /
                                      (uint32_t)nb);

            if (s1 <= s0)
                s1 = s0 + 1;

            double v = NAN;

            /* 柱形取桶内最大值，避免平均把尖峰抹平 */
            for (uint32_t i = s0; i < s1; i++) {
                double cur = series_value(cd, i);

                if (isfinite(cur) && (!isfinite(v) || cur > v))
                    v = cur;
            }

            if (!isfinite(v))
                continue;

            double y = band_y(b, v, sc->lo, sc->hi);

            if (y < b->plot_top)
                y = b->plot_top;
            if (y > bottom)
                y = bottom;

            double h = bottom - y;

            if (h <= 0.0)
                continue;

            double x  = g->left + (double)k * slot + slot * 0.19;
            double bw = slot * 0.62;
            double a  = (k + 1 == nb)
                            ? 0.95
                            : 0.62 + 0.22 * (v / (sc->hi > 0.0 ? sc->hi : 1.0));

            cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, MIN(a, 0.92));
            cairo_rectangle(cr, x, y, bw, h);
            cairo_fill(cr);
        }

        return;
    }

    if (pts == NULL || n == 0)
        return;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_set_line_width(cr, 1.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

    fmon_chart_cairo_step_path(cr, pts, n);
    cairo_stroke(cr);

    draw_combo_threshold(cr, g, b, cd, sc);
    draw_combo_threshold_markers(cr, g, b, cd, sc, pts, n);
}
