/*
 * gui_chart_interact.c - 折线图缩放/框选/平移
 */

#include "gui_chart.h"
#include "gui_chart_internal.h"
#include "chart_geom.h"

#include <gtk/gtk.h>

#include <math.h>

/* 取曲线的最新真实时间；没有时间戳时返回 0。 */
uint64_t curve_latest_us(const CurveData *cd)
{
    if (cd == NULL || cd->count < 2)
        return 0;

    uint64_t t0 = 0, t1 = 0;

    if (!fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS,
                               MIN(cd->count, FMON_HISTORY_POINTS), cd->index,
                               &t0, &t1))
        return 0;

    return t1;
}

static void queue_chart_redraw(GtkWidget *area)
{
    GtkRoot *root = gtk_widget_get_root(area);

    if (root != NULL && GTK_IS_WIDGET(root))
        gtk_widget_queue_draw(GTK_WIDGET(root));
    else
        gtk_widget_queue_draw(area);
}

typedef struct {
    GtkWidget      *area;
    const CurveData *cd;
    double          drag_x;
    uint64_t        drag_end;
    gint64          last_press_us;
} ChartInteract;

static void apply_selection(ChartInteract *ctx)
{
    if (!g_sel_active || ctx->cd == NULL)
        return;

    int width = gtk_widget_get_width(ctx->area);

    if (width <= 80)
        return;

    double a = MIN(g_sel_x0, g_sel_x1) / (double)width;
    double b = MAX(g_sel_x0, g_sel_x1) / (double)width;

    if (a < 0.0)
        a = 0.0;
    if (b > 1.0)
        b = 1.0;

    if (b - a < 0.02)
        return;

    uint32_t n = MIN(ctx->cd->count, FMON_HISTORY_POINTS);
    uint64_t t0 = 0, t1 = 0;

    if (n < 2 || !gui_chart_view_range(ctx->cd, n, &t0, &t1) || t1 <= t0)
        return;

    uint64_t span = t1 - t0;
    uint64_t win = (uint64_t)((double)span * (b - a));

    if (win < 5ULL * 1000000ULL)
        win = 5ULL * 1000000ULL;
    if (win > 20ULL * 60ULL * 1000000ULL)
        win = 20ULL * 60ULL * 1000000ULL;

    uint64_t end = t0 + (uint64_t)((double)span * b);
    uint64_t latest = curve_latest_us(ctx->cd);

    if (latest > 0 && end + win / 20 >= latest)
        end = 0;   /* 选到最右端时继续跟随最新 */

    gui_chart_set_view_end_us(end);
    gui_chart_set_window_us(win);
}

static void clear_selection(void)
{
    g_sel_active = FALSE;
    g_sel_area   = NULL;
}

static gboolean on_chart_scroll(GtkEventControllerScroll *ctrl, double dx,
                                double dy, gpointer data)
{
    (void)ctrl;
    (void)dx;

    ChartInteract *ctx = data;

    static const uint64_t steps[] = {
        60ULL * 1000000ULL,
        5ULL * 60ULL * 1000000ULL,
        15ULL * 60ULL * 1000000ULL,
        20ULL * 60ULL * 1000000ULL,
    };
    const int nsteps = (int)(sizeof(steps) / sizeof(steps[0]));

    uint64_t cur = gui_chart_window_us();
    int      idx = nsteps - 1;

    for (int i = 0; i < nsteps; i++) {
        if (cur <= steps[i]) {
            idx = i;
            break;
        }
    }

    if (dy < 0.0 && idx > 0)
        idx--;
    else if (dy > 0.0 && idx < nsteps - 1)
        idx++;

    gui_chart_set_window_us(steps[idx]);
    queue_chart_redraw(ctx->area);
    return TRUE;
}

static void on_chart_drag_begin(GtkGestureDrag *gesture, double x, double y,
                                gpointer data)
{
    (void)gesture;
    (void)y;

    ChartInteract *ctx = data;

    gint64 now = g_get_monotonic_time();

    if (now - ctx->last_press_us < 350000) {
        gui_chart_reset_view();
        queue_chart_redraw(ctx->area);
        ctx->last_press_us = 0;
        return;
    }

    ctx->last_press_us = now;

    /* 左键：框选。 */
    g_sel_area   = ctx->area;
    g_sel_x0     = x;
    g_sel_x1     = x;
    g_sel_active = TRUE;
    queue_chart_redraw(ctx->area);
}

static void on_chart_drag_update(GtkGestureDrag *gesture, double dx, double dy,
                                 gpointer data)
{
    (void)gesture;
    (void)dy;

    ChartInteract *ctx = data;

    if (!g_sel_active || g_sel_area != ctx->area)
        return;

    g_sel_x1 = g_sel_x0 + dx;
    queue_chart_redraw(ctx->area);
}

static void on_chart_drag_end(GtkGestureDrag *gesture, double dx, double dy,
                              gpointer data)
{
    (void)gesture;
    (void)dy;

    ChartInteract *ctx = data;

    if (!g_sel_active || g_sel_area != ctx->area)
        return;

    g_sel_x1 = g_sel_x0 + dx;

    if (fabs(g_sel_x1 - g_sel_x0) >= 5.0)
        apply_selection(ctx);

    clear_selection();
    queue_chart_redraw(ctx->area);
}

static void on_pan_begin(GtkGestureDrag *gesture, double x, double y,
                         gpointer data)
{
    (void)gesture;
    (void)y;

    ChartInteract *ctx = data;

    ctx->drag_x   = x;
    ctx->drag_end = curve_latest_us(ctx->cd);

    if (ctx->drag_end != 0 && g_chart_view_end_us > 0 &&
        g_chart_view_end_us < ctx->drag_end)
        ctx->drag_end = g_chart_view_end_us;
}

static void on_pan_update(GtkGestureDrag *gesture, double dx, double dy,
                          gpointer data)
{
    (void)gesture;
    (void)dy;

    ChartInteract *ctx = data;

    if (ctx->cd == NULL || ctx->drag_end == 0)
        return;

    int width = gtk_widget_get_width(ctx->area);

    if (width <= 80)
        return;

    uint64_t span = gui_chart_window_us();

    if (span == 0)
        span = 20ULL * 60ULL * 1000000ULL;

    double  frac = dx / (double)(width - 80);
    int64_t delta = (int64_t)(frac * (double)span);
    int64_t end = (int64_t)ctx->drag_end - delta;

    if (end < 0)
        end = 0;

    gui_chart_set_view_end_us((uint64_t)end);
    queue_chart_redraw(ctx->area);
}

static void on_pan_end(GtkGestureDrag *gesture, double dx, double dy,
                       gpointer data)
{
    (void)gesture;
    (void)dx;
    (void)dy;

    ChartInteract *ctx = data;

    gui_chart_set_window_us(gui_chart_window_us());
    queue_chart_redraw(ctx->area);
}

void gui_chart_attach_interactions(GtkWidget *area, const CurveData *cd)
{
    if (area == NULL || g_object_get_data(G_OBJECT(area), "fmon-chart-zoom"))
        return;

    ChartInteract *ctx = g_new0(ChartInteract, 1);

    ctx->area = area;
    ctx->cd   = cd;

    g_object_set_data_full(G_OBJECT(area), "fmon-chart-zoom", ctx, g_free);

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    g_signal_connect(scroll, "scroll", G_CALLBACK(on_chart_scroll), ctx);
    gtk_widget_add_controller(area, scroll);

    GtkGesture *drag = gtk_gesture_drag_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_chart_drag_begin), ctx);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_chart_drag_update), ctx);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_chart_drag_end), ctx);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));

    /* 右键拖动平移。 */
    GtkGesture *pan = gtk_gesture_drag_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan), 3);
    g_signal_connect(pan, "drag-begin", G_CALLBACK(on_pan_begin), ctx);
    g_signal_connect(pan, "drag-update", G_CALLBACK(on_pan_update), ctx);
    g_signal_connect(pan, "drag-end", G_CALLBACK(on_pan_end), ctx);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(pan));

    gtk_widget_set_tooltip_text(
        area, "滚轮缩放 · 左键框选 · 右键拖动平移 · 双击还原");
}
