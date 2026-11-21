/*
 * combo_chart.c - 多曲线合并图表实现
 *
 * 每个条带是一条独立的线性折线；条带之间共享同一条时间轴。
 * 曲线绘制只使用真实采样点，缺失值断开，不补 0、不做样条平滑。
 */

#include "combo_chart.h"
#include "combo_chart_internal.h"

#include "gui_util.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void draw_band_hint(cairo_t *cr, const ComboGeom *g,
                           const BandGeom *b, const CurveData *cd)
{
    const char *hint = cd->empty_hint;

    if (hint == NULL)
        hint = (cd->count < 2) ? "正在采集…" : "暂无有效采样";

    double axis[4];

    fmon_theme_axis_color(axis);
    set_font(cr, 11.0);

    cairo_text_extents_t ext;

    cairo_text_extents(cr, hint, &ext);

    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.75);
    cairo_move_to(cr, g->left + (g->plot_w - ext.width) / 2.0,
                  b->plot_top + (b->plot_h + ext.height) / 2.0);
    cairo_show_text(cr, hint);
}

typedef struct {
    const CurveData *cd;
    const double    *history;
    const uint64_t  *time_us;
    uint32_t count;
    uint32_t index;
    uint64_t t0;
    uint64_t t1;
    uint64_t end_time;
    uint64_t gap_us;
    double   left;
    double   top;
    double   plot_w;
    double   plot_h;
    double   lo;
    double   hi;
    uint32_t max_cols;

    FmonChartPoint *pts;
    size_t          n;
} ComboPointCache;

typedef struct {
    ComboPointCache series[4];
} ComboCache;

static void combo_cache_free(gpointer data)
{
    ComboCache *cache = data;

    if (cache == NULL)
        return;

    for (int i = 0; i < 4; i++)
        g_free(cache->series[i].pts);

    g_free(cache);
}

static ComboCache *combo_cache_get(GtkWidget *area)
{
    ComboCache *cache =
        g_object_get_data(G_OBJECT(area), "fmon-combo-point-cache");

    if (cache == NULL) {
        cache = g_new0(ComboCache, 1);
        g_object_set_data_full(G_OBJECT(area), "fmon-combo-point-cache",
                               cache, combo_cache_free);
    }

    return cache;
}

static bool combo_cache_match(const ComboPointCache *c, const CurveData *cd,
                              uint64_t t0, uint64_t t1, uint64_t gap_us,
                              double left, double top, double plot_w,
                              double plot_h, double lo, double hi,
                              uint32_t max_cols)
{
    return c->pts != NULL && c->cd == cd && c->history == cd->history &&
           c->time_us == cd->time_us && c->count == cd->count &&
           c->index == cd->index && c->t0 == t0 && c->t1 == t1 &&
           c->end_time == cd->end_time_us && c->gap_us == gap_us &&
           c->left == left && c->top == top && c->plot_w == plot_w &&
           c->plot_h == plot_h && c->lo == lo && c->hi == hi &&
           c->max_cols == max_cols;
}

static void combo_cache_invalidate(ComboPointCache *c)
{
    g_free(c->pts);
    c->pts = NULL;
    c->n = 0;
}

void combo_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer data)
{
    if (getenv("FMON_NO_CHART_DRAW") != NULL)
        return;

    ComboChart *c = data;

    if (cr == NULL || c == NULL)
        return;

    ComboGeom g;
    int       nseries = c->n;

    if (nseries < 0)
        nseries = 0;
    if (nseries > COMBO_MAX_SERIES)
        nseries = COMBO_MAX_SERIES;

    combo_geom_init(&g, width, height);

    if (g.plot_w < 20.0 || g.plot_h < 20.0 || nseries == 0)
        return;

    draw_grid(cr, &g, nseries);
    draw_axis_labels(cr, &g, c);

    ComboCache *cache = combo_cache_get(GTK_WIDGET(area));

    for (int s = 0; s < nseries; s++) {
        const CurveData *cd = c->series[s].cd;

        if (cd == NULL)
            continue;

        BandGeom b;

        band_layout(&g, c->n, s, &b);
        draw_band_frame(cr, &g, &b, cd);
        draw_band_label(cr, &g, &b, &c->series[s], cd);

        FmonChartScale sc;
        bool           have = series_scale(cd, &sc);

        draw_band_ticks(cr, &g, &b, &sc, cd->unit);

        if (cd->count < 2 || !have) {
            draw_band_hint(cr, &g, &b, cd);
            continue;
        }

        FmonChartPoint *pts = NULL;
        size_t          n = 0;

        if (!cd->bars && cd->count >= 2) {
            uint64_t t0 = 0, t1 = 0;

            if (!gui_chart_view_range(cd, cd->count, &t0, &t1))
                fmon_chart_time_range(cd->time_us, COMBO_MAX_POINTS,
                                      cd->count, cd->index, &t0, &t1);

            uint64_t gap_us = cd->interval_us * 5u / 2u;
            uint32_t max_cols = (uint32_t)g.plot_w;
            ComboPointCache *cc = &cache->series[s];

            if (combo_cache_match(cc, cd, t0, t1, gap_us, g.left,
                                  b.plot_top, g.plot_w, b.plot_h, sc.lo,
                                  sc.hi, max_cols)) {
                pts = cc->pts;
                n = cc->n;
            } else {
                combo_cache_invalidate(cc);

                size_t cap = (size_t)((cd->count > COMBO_MAX_POINTS)
                                          ? COMBO_MAX_POINTS
                                          : cd->count) +
                             4;

                cc->pts = g_new0(FmonChartPoint, cap);
                n = fmon_chart_polyline_at(
                    cd->history, cd->time_us, COMBO_MAX_POINTS, cd->count,
                    cd->index, t0, t1, g.left, g.plot_w, b.plot_top,
                    b.plot_h, sc.lo, sc.hi, max_cols, gap_us, cc->pts, cap);

                cc->n = n;
                cc->cd = cd;
                cc->history = cd->history;
                cc->time_us = cd->time_us;
                cc->count = cd->count;
                cc->index = cd->index;
                cc->t0 = t0;
                cc->t1 = t1;
                cc->end_time = cd->end_time_us;
                cc->gap_us = gap_us;
                cc->left = g.left;
                cc->top = b.plot_top;
                cc->plot_w = g.plot_w;
                cc->plot_h = b.plot_h;
                cc->lo = sc.lo;
                cc->hi = sc.hi;
                cc->max_cols = max_cols;

                pts = cc->pts;
            }
        }

        cairo_save(cr);
        cairo_rectangle(cr, g.left, b.plot_top, g.plot_w, b.plot_h);
        cairo_clip(cr);

        draw_series_line(cr, &g, &b, cd, &sc, pts, n);

        cairo_restore(cr);
    }

    gui_chart_draw_selection(cr, GTK_WIDGET(area), g.left, g.top, g.plot_w,
                             g.plot_h);

    if (gui_chart_auto_range()) {
        gboolean pct = FALSE;

        for (int s = 0; s < nseries; s++) {
            const CurveData *cd = c->series[s].cd;

            if (cd != NULL && cd->unit != NULL && strcmp(cd->unit, "%") == 0) {
                pct = TRUE;
                break;
            }
        }

        if (pct) {
            double axis[4];

            fmon_theme_axis_color(axis);
            set_font(cr, 9.0);

            const char *tag = "自动量程";
            cairo_text_extents_t ext;

            cairo_text_extents(cr, tag, &ext);
            cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.60);
            cairo_move_to(cr, g.left + g.plot_w - ext.width - 4.0,
                          g.top + 11.0);
            cairo_show_text(cr, tag);
        }
    }

    draw_hover(cr, &g, c);
}

void combo_chart_motion(GtkEventControllerMotion *ctrl, double x, double y,
                        gpointer data)
{
    ComboChart *c = data;
    GtkWidget  *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));

    c->hovering = TRUE;
    c->hover_x  = x;
    c->hover_y  = y;
    gtk_widget_queue_draw(w);
}

void combo_chart_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    ComboChart *c = data;
    GtkWidget  *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));

    c->hovering = FALSE;
    gtk_widget_queue_draw(w);
}
