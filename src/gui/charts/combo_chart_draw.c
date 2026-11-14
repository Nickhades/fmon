/*
 * combo_chart_draw.c - 组合图表绘制（网格/带框/曲线/悬停）
 */

#include "combo_chart_internal.h"

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
    const CurveData *cd0;

    if (c->n == 0)
        return;

    cd0 = c->series[0].cd;

    if (cd0->count < 2)
        return;

    int64_t span = (int64_t)(cd0->count - 1) *
                   (int64_t)(cd0->interval_us) / 1000000;

    set_font(cr, 10.0);

    for (int i = 0; i < 4; i++) {
        double x = g->left + g->plot_w * (double)i / 3.0;
        char   buf[32];

        if (i == 3)
            g_snprintf(buf, sizeof(buf), "现在");
        else
            format_span(buf, sizeof(buf), span - span * i / 3);

        double axis[4];

        fmon_theme_axis_color(axis);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], axis[3]);
        cairo_move_to(cr, x - (i == 3 ? 22.0 : 10.0),
                      g->top + g->plot_h + 16.0);
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
    char   label[96];
    char   cur_txt[64];
    double cur;

    /* 1 秒一个采样点：带名后的数值就是最近这一秒的值 */
    cur = (cd->count > 0) ? series_value(cd, cd->count - 1) : 0.0;

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

void draw_series_fill(cairo_t *cr, const ComboGeom *g,
                             const BandGeom *b, const CurveData *cd,
                             double lo, double hi)
{
    if (cd->count == 0 || cd->bars)
        return;   /* 柱形图不需要面积填充 */

    double plot_bottom = b->plot_top + b->plot_h;

    cairo_pattern_t *grad = cairo_pattern_create_linear(0.0, b->plot_top, 0.0,
                                                        plot_bottom);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, cd->r, cd->g, cd->b, 0.30);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, cd->r, cd->g, cd->b, 0.02);

    double prev_y = band_y(b, series_value(cd, 0), lo, hi);

    cairo_move_to(cr, g->left, plot_bottom);
    cairo_line_to(cr, g->left, prev_y);

    for (uint32_t i = 1; i < cd->count; i++) {
        double t = (double)i / (double)(cd->count - 1);
        double x = g->left + t * g->plot_w;
        double y = band_y(b, series_value(cd, i), lo, hi);

        /* 阶梯填充：先水平保持，再垂直跳变 */
        cairo_line_to(cr, x, prev_y);
        cairo_line_to(cr, x, y);
        prev_y = y;
    }

    cairo_line_to(cr, g->left + g->plot_w, plot_bottom);
    cairo_close_path(cr);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);
}

void draw_series_line(cairo_t *cr, const ComboGeom *g,
                             const BandGeom *b, const CurveData *cd,
                             double lo, double hi)
{
    if (cd->count == 0)
        return;

    if (cd->bars) {
        double bottom = b->plot_top + b->plot_h;

        /* 排行榜式柱形：按桶聚合，柱间留缝，底座固定在 0 */
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

            double acc = 0.0;

            for (uint32_t i = s0; i < s1; i++)
                acc += series_value(cd, i);

            double v = acc / (double)(s1 - s0);
            double y = band_y(b, v, lo, hi);

            if (y < b->plot_top)
                y = b->plot_top;

            double h = bottom - y;

            if (h <= 0.0)
                continue;

            double x  = g->left + (double)k * slot + slot * 0.19;
            double bw = slot * 0.62;
            double a  = (k + 1 == nb) ? 0.95
                                      : 0.62 + 0.22 * (v / (hi > 0.0 ? hi : 1.0));

            cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, MIN(a, 0.92));
            cairo_rectangle(cr, x, y, bw, h);
            cairo_fill(cr);
        }

        return;
    }


    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.95);
    cairo_set_line_width(cr, 1.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

    double prev_y = band_y(b, series_value(cd, 0), lo, hi);

    cairo_move_to(cr, g->left, prev_y);

    for (uint32_t i = 1; i < cd->count; i++) {
        double t = (double)i / (double)(cd->count - 1);
        double x = g->left + t * g->plot_w;
        double y = band_y(b, series_value(cd, i), lo, hi);

        /* 直角阶梯：水平保持到下一采样时刻，再垂直跳变 */
        cairo_line_to(cr, x, prev_y);
        cairo_line_to(cr, x, y);
        prev_y = y;
    }

    cairo_stroke(cr);
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
    for (int s = 0; s < c->n; s++) {
        const CurveData *cd = c->series[s].cd;
        BandGeom         b;
        double           lo;
        double           hi;
        uint32_t         ci = idx;

        if (cd->count < 2)
            continue;

        if (ci >= cd->count)
            ci = cd->count - 1;

        series_range(cd, &lo, &hi);
        band_layout(g, c->n, s, &b);

        cairo_arc(cr, x, band_y(&b, series_value(cd, ci), lo, hi), 3.5, 0.0,
                  COMBO_TAU);
        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 1.0);
        cairo_fill(cr);
    }
}

void draw_hover_box(cairo_t *cr, const ComboGeom *g,
                           const ComboChart *c, uint32_t idx)
{
    double bw = 220.0;
    double bh = 18.0 + 19.0 * (double)c->n;
    double bx = c->hover_x + 16.0;
    double by = (c->hover_y > 0.0) ? c->hover_y + 14.0 : g->top + 8.0;

    if (bx + bw > g->left + g->plot_w)
        bx = c->hover_x - 16.0 - bw;
    if (bx < g->left)
        bx = g->left;

    if (by + bh > g->top + g->plot_h)
        by = c->hover_y - 14.0 - bh;
    if (by < g->top)
        by = g->top;

    double hb[4];

    fmon_theme_hover_bg(hb);
    cairo_set_source_rgba(cr, hb[0], hb[1], hb[2], hb[3]);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_fill(cr);

    set_font(cr, 12.5);

    for (int s = 0; s < c->n; s++) {
        const CurveData *cd = c->series[s].cd;
        char             buf[96];
        char             val_txt[64];

        format_value_text(val_txt, sizeof(val_txt), series_value(cd, idx),
                          cd->unit);
        g_snprintf(buf, sizeof(buf), "%s  %s", c->series[s].name, val_txt);

        double tr, tg, tb;

        series_text_color(cd, &tr, &tg, &tb);
        cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
        cairo_move_to(cr, bx + 12.0, by + 21.0 + 19.0 * (double)s);
        cairo_show_text(cr, buf);
    }
}

void draw_hover(cairo_t *cr, const ComboGeom *g, const ComboChart *c)
{
    const CurveData *cd0;
    double           t;
    uint32_t         idx;
    double           x;

    if (!c->hovering || c->n == 0)
        return;

    if (c->hover_x < g->left || c->hover_x > g->left + g->plot_w)
        return;

    cd0 = c->series[0].cd;

    if (cd0->count <= 1)
        return;

    t   = (c->hover_x - g->left) / g->plot_w;
    idx = (uint32_t)(t * (double)(cd0->count - 1));

    if (idx >= cd0->count)
        idx = cd0->count - 1;

    x = g->left + g->plot_w * t;

    draw_hover_line(cr, g, x);
    draw_hover_dots(cr, g, c, idx, x);
    draw_hover_box(cr, g, c, idx);
}







