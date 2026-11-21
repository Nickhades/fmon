/*
 * combo_chart_hover.c - 组合图悬停十字线/读数框
 */

#include "combo_chart_internal.h"

#include "theme.h"

#include <cairo.h>

#include <math.h>

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
    bool has_time = gui_chart_view_range(cd0, n, &t0, &t1);

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
