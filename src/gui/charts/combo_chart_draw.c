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
        double x = g->left + pw * (double)i / 3.0;

        fmon_theme_grid_color(line, FALSE);
        cairo_set_source_rgba(cr, line[0], line[1], line[2], line[3]);
        cairo_move_to(cr, x, g->top);
        cairo_line_to(cr, x, g->top + ph);
        cairo_stroke(cr);
    }

    for (int s = 1; s < series_count; s++) {
        double y = g->top + ph * (double)s / (double)series_count;

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
    bool has_time = fmon_chart_time_range(cd0->time_us, COMBO_MAX_POINTS, n,
                                          cd0->index, &t0, &t1);
    double span_sec = has_time
                          ? (double)(t1 - t0) / 1000000.0
                          : (double)(n - 1) * (double)cd0->interval_us /
                                1000000.0;

    set_font(cr, 10.0);

    for (int i = 0; i < 4; i++) {
        double x = g->left + g->plot_w * (double)i / 3.0;
        char   buf[32];

        if (i == 3)
            gui_chart_format_time_label(buf, sizeof(buf), 0.0);
        else
            gui_chart_format_time_label(buf, sizeof(buf),
                                        span_sec * (1.0 - (double)i / 3.0));

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

void draw_series_fill(cairo_t *cr, const ComboGeom *g,
                      const BandGeom *b, const CurveData *cd,
                      const FmonChartScale *sc)
{
    if (cd->count < 2 || cd->bars || sc == NULL)
        return;

    size_t cap = (size_t)((cd->count > COMBO_MAX_POINTS) ? COMBO_MAX_POINTS
                                                         : cd->count) +
                 4;
    FmonChartPoint *pts = g_new0(FmonChartPoint, cap);

    uint64_t t0 = 0, t1 = 0;
    fmon_chart_time_range(cd->time_us, COMBO_MAX_POINTS, cd->count, cd->index,
                          &t0, &t1);
    uint64_t gap_us = cd->interval_us * 5u / 2u;

    size_t n = fmon_chart_polyline_at(
        cd->history, cd->time_us, COMBO_MAX_POINTS, cd->count, cd->index, t0,
        t1, g->left, g->plot_w, b->plot_top, b->plot_h, sc->lo, sc->hi,
        (uint32_t)g->plot_w, gap_us, pts, cap);

    if (n == 0) {
        g_free(pts);
        return;
    }

    double plot_bottom = b->plot_top + b->plot_h;

    cairo_pattern_t *grad = cairo_pattern_create_linear(0.0, b->plot_top, 0.0,
                                                        plot_bottom);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, cd->r, cd->g, cd->b, 0.30);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, cd->r, cd->g, cd->b, 0.02);

    fmon_chart_cairo_fill(cr, pts, n, plot_bottom);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    g_free(pts);
}

void draw_series_line(cairo_t *cr, const ComboGeom *g,
                      const BandGeom *b, const CurveData *cd,
                      const FmonChartScale *sc)
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

    size_t cap = (size_t)((cd->count > COMBO_MAX_POINTS) ? COMBO_MAX_POINTS
                                                         : cd->count) +
                 4;
    FmonChartPoint *pts = g_new0(FmonChartPoint, cap);

    uint64_t t0 = 0, t1 = 0;
    fmon_chart_time_range(cd->time_us, COMBO_MAX_POINTS, cd->count, cd->index,
                          &t0, &t1);
    uint64_t gap_us = cd->interval_us * 5u / 2u;

    size_t n = fmon_chart_polyline_at(
        cd->history, cd->time_us, COMBO_MAX_POINTS, cd->count, cd->index, t0,
        t1, g->left, g->plot_w, b->plot_top, b->plot_h, sc->lo, sc->hi,
        (uint32_t)g->plot_w, gap_us, pts, cap);

    if (n == 0) {
        g_free(pts);
        return;
    }

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.95);
    cairo_set_line_width(cr, 1.35);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

    fmon_chart_cairo_path(cr, pts, n);
    cairo_stroke(cr);

    g_free(pts);
}

void draw_hover_line(cairo_t *cr, const ComboGeom *g, double x)
{
    double c[4];

    fmon_theme_hover_line(c);
    cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x, g->top);
    cairo_line_to(cr, x, g->top + g->plot_h);
    cairo_stroke(cr);
}

void draw_hover_dots(cairo_t *cr, const ComboGeom *g,
                     const ComboChart *c, uint32_t idx, double x)
{
    int nseries = (c->n > COMBO_MAX_SERIES) ? COMBO_MAX_SERIES : c->n;

    if (nseries < 0)
        nseries = 0;

    for (int s = 0; s < nseries; s++) {
        const CurveData *cd = c->series[s].cd;

        if (cd->count < 2 || idx >= cd->count)
            continue;

        double v = series_value(cd, idx);

        if (!isfinite(v))
            continue;

        FmonChartScale sc;

        if (!series_scale(cd, &sc))
            continue;

        BandGeom b;

        band_layout(g, c->n, s, &b);

        cairo_arc(cr, x, band_y(&b, v, sc.lo, sc.hi), 3.5, 0.0, COMBO_TAU);
        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 1.0);
        cairo_fill(cr);
    }
}

void draw_hover_box(cairo_t *cr, const ComboGeom *g,
                    const ComboChart *c, uint32_t idx)
{
    set_font(cr, 12.5);

    char  lines[COMBO_MAX_SERIES][128];
    double maxw = 0.0;
    int    nlines = (c->n < COMBO_MAX_SERIES) ? c->n : COMBO_MAX_SERIES;

    if (nlines < 0)
        nlines = 0;

    for (int s = 0; s < nlines; s++) {
        const CurveData *cd = c->series[s].cd;
        char             val_txt[64];

        double v = (idx < cd->count) ? series_value(cd, idx) : NAN;

        format_value_text(val_txt, sizeof(val_txt), v, cd->unit);
        g_snprintf(lines[s], sizeof(lines[s]), "%s  %s",
                   c->series[s].name, val_txt);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, lines[s], &ext);
        if (ext.width > maxw)
            maxw = ext.width;
    }

    double bw = maxw + 24.0;
    double bh = 18.0 + 19.0 * (double)nlines;

    if (bw < 180.0)
        bw = 180.0;

    double bx = c->hover_x + 16.0;
    double by = (c->hover_y > 0.0) ? c->hover_y + 14.0 : g->top + 8.0;

    if (bx + bw > g->left + g->plot_w)
        bx = c->hover_x - 16.0 - bw;
    if (bx < g->left)
        bx = g->left;
    if (bx + bw > g->left + g->plot_w)
        bw = g->left + g->plot_w - bx;

    if (by + bh > g->top + g->plot_h)
        by = c->hover_y - 14.0 - bh;
    if (by < g->top)
        by = g->top;

    double hb[4];

    fmon_theme_hover_bg(hb);
    cairo_set_source_rgba(cr, hb[0], hb[1], hb[2], hb[3]);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_fill(cr);

    double border[4];

    fmon_theme_grid_color(border, TRUE);
    cairo_set_source_rgba(cr, border[0], border[1], border[2], 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_stroke(cr);

    for (int s = 0; s < nlines; s++) {
        double tr, tg, tb;

        series_text_color(c->series[s].cd, &tr, &tg, &tb);
        cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
        cairo_move_to(cr, bx + 12.0, by + 21.0 + 19.0 * (double)s);
        cairo_show_text(cr, lines[s]);
    }
}

static bool combo_nearest_by_time(const CurveData *cd, uint32_t n,
                                  uint64_t wanted, uint32_t *out_idx,
                                  uint64_t *out_time)
{
    uint64_t best   = UINT64_MAX;
    uint32_t best_i = 0;
    uint64_t best_t = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t ti;
        double   v;

        if (!fmon_chart_history_time(cd->time_us, COMBO_MAX_POINTS, n,
                                     cd->index, i, &ti))
            continue;
        if (!fmon_chart_history_value(cd->history, COMBO_MAX_POINTS, n,
                                      cd->index, i, &v))
            continue;

        uint64_t d = (ti > wanted) ? ti - wanted : wanted - ti;

        if (d < best) {
            best   = d;
            best_i = i;
            best_t = ti;
        }
    }

    if (best == UINT64_MAX)
        return false;

    *out_idx  = best_i;
    *out_time = best_t;
    return true;
}

void draw_hover(cairo_t *cr, const ComboGeom *g, const ComboChart *c)
{
    if (!c->hovering || c->n <= 0)
        return;

    if (c->hover_x < g->left || c->hover_x > g->left + g->plot_w)
        return;

    const CurveData *cd0 = c->series[0].cd;
    uint32_t n = (cd0->count > COMBO_MAX_POINTS) ? COMBO_MAX_POINTS
                                                 : cd0->count;

    if (n <= 1)
        return;

    double t = (c->hover_x - g->left) / g->plot_w;

    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;

    uint32_t idx = 0;
    double   x;
    uint64_t t0 = 0, t1 = 0;
    bool has_time = fmon_chart_time_range(cd0->time_us, COMBO_MAX_POINTS, n,
                                          cd0->index, &t0, &t1);

    if (has_time) {
        uint64_t wanted = t0 + (uint64_t)(t * (double)(t1 - t0));
        uint64_t ti = 0;

        if (!combo_nearest_by_time(cd0, n, wanted, &idx, &ti)) {
            idx = (uint32_t)floor(t * (double)(n - 1) + 0.5);
            ti  = t0;
        }

        x = fmon_chart_time_x(ti, t0, t1, g->left, g->plot_w);
    } else {
        idx = (uint32_t)floor(t * (double)(n - 1) + 0.5);

        if (idx >= n)
            idx = n - 1;

        x = g->left + g->plot_w * (double)idx / (double)(n - 1);
    }

    /* 十字线吸附到真实采样点：读数与点的位置严格一致，不对读数做插值。 */
    draw_hover_line(cr, g, x);
    draw_hover_dots(cr, g, c, idx, x);
    draw_hover_box(cr, g, c, idx);
}
