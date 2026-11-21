/*
 * gui_chart_view.c - 折线图时间窗口与框选状态
 */

#include "gui_chart.h"
#include "gui_chart_internal.h"

#include "chart_geom.h"

#include <gtk/gtk.h>

uint64_t g_chart_window_us;
uint64_t g_chart_view_end_us;
static GuiChartWindowNotify g_window_notify;
GtkWidget *g_sel_area;
double     g_sel_x0;
double     g_sel_x1;
gboolean   g_sel_active;

void gui_chart_set_window_notify(GuiChartWindowNotify cb)
{
    g_window_notify = cb;
}

static void chart_notify_window(void)
{
    if (g_window_notify != NULL)
        g_window_notify(g_chart_window_us);
}

uint64_t gui_chart_window_us(void)
{
    return g_chart_window_us;
}

void gui_chart_set_window_us(uint64_t us)
{
    g_chart_window_us = us;
    chart_notify_window();
}

void gui_chart_set_view_end_us(uint64_t us)
{
    g_chart_view_end_us = us;
}

void gui_chart_reset_view(void)
{
    g_chart_view_end_us = 0;
    g_chart_window_us  = 20ULL * 60ULL * 1000000ULL;
    chart_notify_window();
}

gboolean gui_chart_view_range(const CurveData *cd, uint32_t points,
                              uint64_t *t0, uint64_t *t1)
{
    if (cd == NULL || points < 2 || t0 == NULL || t1 == NULL)
        return FALSE;

    uint32_t n = points;

    if (n > FMON_HISTORY_POINTS)
        n = FMON_HISTORY_POINTS;

    if (!fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS, n, cd->index,
                               t0, t1))
        return FALSE;

    /* 历史页局部框选：直接限制到选中的绝对时间，不叠加实时窗口。 */
    if (cd->view_start_us != 0 || cd->view_end_us != 0) {
        uint64_t first = *t0;
        uint64_t last  = *t1;

        if (cd->view_start_us > first)
            *t0 = cd->view_start_us;
        if (cd->view_end_us != 0 && cd->view_end_us < last)
            *t1 = cd->view_end_us;
        if (*t1 <= *t0)
            *t0 = first;
        return TRUE;
    }

    /* 历史页自己用 1h/24h 选择器，不再叠加实时图窗口。 */
    if (cd->fixed_range)
        return TRUE;

    uint64_t first  = *t0;
    uint64_t data_latest = *t1;
    uint64_t latest = data_latest;

    if (g_chart_view_end_us > 0 && g_chart_view_end_us < latest) {
        latest = g_chart_view_end_us;

        /* 平移到最早端时仍保留一个完整窗口，避免空白。 */
        if (g_chart_window_us > 0 &&
            latest < first + g_chart_window_us)
            latest = first + g_chart_window_us;
        if (latest > data_latest)
            latest = data_latest;
    }
    if (latest < first)
        latest = first;

    if (g_chart_window_us > 0) {
        if (latest > first + g_chart_window_us)
            *t0 = latest - g_chart_window_us;
        else
            *t0 = first;
    }

    *t1 = latest;
    return TRUE;
}

void gui_chart_selection_set(GtkWidget *area, double x0, double x1,
                             gboolean active)
{
    g_sel_active = active;
    g_sel_area   = active ? area : NULL;
    g_sel_x0     = x0;
    g_sel_x1     = x1;
}

void gui_chart_draw_selection(cairo_t *cr, GtkWidget *area, double left,
                                double top, double w, double h)
{
    if (!g_sel_active || area == NULL || g_sel_area != area)
        return;

    double x0 = MIN(g_sel_x0, g_sel_x1);
    double x1 = MAX(g_sel_x0, g_sel_x1);

    x0 = MAX(x0, left);
    x1 = MIN(x1, left + w);

    if (x1 - x0 < 1.0)
        return;

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.20, 0.50, 0.90, 0.16);
    cairo_rectangle(cr, x0, top, x1 - x0, h);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.20, 0.50, 0.90, 0.55);
    cairo_set_line_width(cr, 1.0);
    double dash[] = { 4.0, 3.0 };
    cairo_set_dash(cr, dash, 2, 0.0);
    cairo_rectangle(cr, x0 + 0.5, top + 0.5, x1 - x0 - 1.0, h - 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}
