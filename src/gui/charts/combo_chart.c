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

void combo_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer data)
{
    (void)area;

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

        cairo_save(cr);
        cairo_rectangle(cr, g.left, b.plot_top, g.plot_w, b.plot_h);
        cairo_clip(cr);

        draw_series_fill(cr, &g, &b, cd, &sc);
        draw_series_line(cr, &g, &b, cd, &sc);

        cairo_restore(cr);
    }

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
