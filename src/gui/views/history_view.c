/*
 * history_view.c - 指标历史页面
 *
 * 读取守护进程每 10 秒写入的 history.tsv，按 1 小时 / 24 小时范围
 * 降采样到最多 FMON_HISTORY_POINTS 个点，用曲线展示 CPU 与内存。
 *
 * 24 小时视图每个时间桶保留真实 min/max：曲线本身仍是桶平均，
 * 背景色带显示该桶内 10 秒原始采样的极值，避免平均把尖峰藏起来。
 */

#include "history_view.h"

#include "history_internal.h"

#include "gui_chart.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>
#include <time.h>

static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);

    if (id != 0)
        g_source_remove(id);
}

static void reset_history_view(HistoryPage *page)
{
    page->custom        = FALSE;
    page->view_start_us = 0;
    page->view_end_us   = 0;
    page->cpu_cd.view_start_us = 0;
    page->cpu_cd.view_end_us   = 0;
    page->mem_cd.view_start_us = 0;
    page->mem_cd.view_end_us   = 0;
}

static void on_history_drag_begin(GtkGestureDrag *gesture, double x, double y,
                                  gpointer data)
{
    (void)y;

    HistoryPage *page = data;
    GtkWidget   *area = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));

    gint64 now = g_get_monotonic_time();

    if (now - page->last_press_us < 350000) {
        reset_history_view(page);
        gui_chart_selection_set(area, x, x, FALSE);
        gtk_widget_queue_draw(page->cpu_area);
        gtk_widget_queue_draw(page->mem_area);
        history_refresh(page);
        page->last_press_us = 0;
        return;
    }

    page->last_press_us = now;
    page->drag_x0       = x;
    gui_chart_selection_set(area, x, x, TRUE);
    gtk_widget_queue_draw(page->cpu_area);
    gtk_widget_queue_draw(page->mem_area);
}

static void on_history_drag_update(GtkGestureDrag *gesture, double dx,
                                   double dy, gpointer data)
{
    (void)dy;

    HistoryPage *page = data;
    GtkWidget   *area = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));

    gui_chart_selection_set(area, page->drag_x0, page->drag_x0 + dx, TRUE);
    gtk_widget_queue_draw(page->cpu_area);
    gtk_widget_queue_draw(page->mem_area);
}

static void on_history_drag_end(GtkGestureDrag *gesture, double dx, double dy,
                                gpointer data)
{
    (void)dy;

    HistoryPage *page = data;
    GtkWidget   *area = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));

    gui_chart_selection_set(area, 0, 0, FALSE);
    gtk_widget_queue_draw(page->cpu_area);
    gtk_widget_queue_draw(page->mem_area);

    if (fabs(dx) < 5.0 || page->n < 2)
        return;

    int width = gtk_widget_get_width(area);

    if (width <= 60)
        return;

    double a = MIN(page->drag_x0, page->drag_x0 + dx) / (double)width;
    double b = MAX(page->drag_x0, page->drag_x0 + dx) / (double)width;

    if (a < 0.0)
        a = 0.0;
    if (b > 1.0)
        b = 1.0;
    if (b - a < 0.01)
        return;

    uint64_t first = g_time_us[0];
    uint64_t last  = g_time_us[page->n - 1];
    uint64_t span  = last - first;

    if (span == 0)
        return;

    page->custom        = TRUE;
    page->view_start_us = first + (uint64_t)((double)span * a);
    page->view_end_us   = first + (uint64_t)((double)span * b);

    history_refresh(page);

    char *text = g_strdup_printf(
        "框选窗口：%u 个点（双击图表恢复完整范围；导出仍按下拉框范围）",
        page->n);
    gtk_label_set_text(GTK_LABEL(page->summary), text);
    g_free(text);
}

static gboolean on_history_scroll(GtkEventControllerScroll *ctrl, double dx,
                                  double dy, gpointer data)
{
    (void)ctrl;
    (void)dx;

    HistoryPage *page = data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->range_dd));

    if (dy < 0.0 && sel > 0)
        sel--;
    else if (dy > 0.0 && sel < 3)
        sel++;
    else
        return TRUE;

    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->range_dd), sel);
    return TRUE;
}

static void on_range_changed(GtkDropDown *dd, GParamSpec *pspec,
                             gpointer data)
{
    (void)dd;
    (void)pspec;

    reset_history_view(data);
    history_refresh(data);
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    history_refresh(data);
}

static gboolean on_tick(gpointer data)
{
    HistoryPage *page = data;

    if (page->root == NULL || !gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    history_refresh(page);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *make_card(const char *title, GtkWidget **area_out,
                            CurveData *cd, gdouble r, gdouble g, gdouble b)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card");

    GtkWidget *label = gtk_label_new(title);
    gtk_widget_add_css_class(label, "title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), label);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 170);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_box_append(GTK_BOX(card), area);

    cd->max_val    = 100.0;
    cd->r          = r;
    cd->g          = g;
    cd->b          = b;
    cd->unit       = "%";
    cd->empty_hint = "没有历史数据";

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), gui_chart_draw, cd,
                                   NULL);

    *area_out = area;
    return card;
}

GtkWidget *gui_history_view_new(void)
{
    HistoryPage *page = g_new0(HistoryPage, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_add_css_class(vbox, "view-card");
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_refresh = gtk_button_new_with_label("刷新");
    gtk_widget_add_css_class(btn_refresh, "suggested-action");

    const char *ranges[] = {
        "最近 5 分钟", "最近 15 分钟", "最近 1 小时", "最近 24 小时", NULL
    };
    page->range_dd = gtk_drop_down_new_from_strings(ranges);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->range_dd), 2);

    GtkWidget *btn_export = gtk_button_new_with_label(_("导出 CSV"));

    gtk_box_append(GTK_BOX(toolbar), btn_refresh);
    gtk_box_append(GTK_BOX(toolbar), page->range_dd);
    gtk_box_append(GTK_BOX(toolbar), btn_export);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    page->summary = gtk_label_new("加载中…");
    gtk_widget_add_css_class(page->summary, "small-text");
    gtk_widget_set_halign(page->summary, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), page->summary);

    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_vexpand(cards, TRUE);

    gtk_box_append(GTK_BOX(cards),
                   make_card("CPU 历史 · 平均 + 极值带", &page->cpu_area,
                             &page->cpu_cd, 0.30, 0.85, 0.45));
    gtk_box_append(GTK_BOX(cards),
                   make_card("内存历史 · 平均 + 极值带", &page->mem_area,
                             &page->mem_cd, 0.35, 0.65, 0.95));
    gtk_box_append(GTK_BOX(vbox), cards);

    GtkWidget *hint = gtk_label_new(
        "历史数据由守护进程每 10 秒写入 /var/db/fmon/history.tsv（普通用户调试时为 ~/.local/state/fmon/history.tsv）。");
    gtk_widget_add_css_class(hint, "small-text");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), hint);

    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     page);
    g_signal_connect(btn_export, "clicked",
                     G_CALLBACK(history_export_clicked), page);
    g_signal_connect(page->range_dd, "notify::selected",
                     G_CALLBACK(on_range_changed), page);

    GtkEventController *scroll_cpu =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    g_signal_connect(scroll_cpu, "scroll", G_CALLBACK(on_history_scroll),
                     page);
    gtk_widget_add_controller(page->cpu_area, scroll_cpu);
    gtk_widget_set_tooltip_text(page->cpu_area,
                                _("滚轮切换 5分钟/15分钟/1小时/24小时"));

    GtkEventController *scroll_mem =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    g_signal_connect(scroll_mem, "scroll", G_CALLBACK(on_history_scroll),
                     page);
    gtk_widget_add_controller(page->mem_area, scroll_mem);
    gtk_widget_set_tooltip_text(page->mem_area,
                                _("滚轮切换 5分钟/15分钟/1小时/24小时"));

    GtkGesture *drag_cpu = gtk_gesture_drag_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_cpu), 1);
    g_signal_connect(drag_cpu, "drag-begin",
                     G_CALLBACK(on_history_drag_begin), page);
    g_signal_connect(drag_cpu, "drag-update",
                     G_CALLBACK(on_history_drag_update), page);
    g_signal_connect(drag_cpu, "drag-end",
                     G_CALLBACK(on_history_drag_end), page);
    gtk_widget_add_controller(page->cpu_area, GTK_EVENT_CONTROLLER(drag_cpu));

    GtkGesture *drag_mem = gtk_gesture_drag_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_mem), 1);
    g_signal_connect(drag_mem, "drag-begin",
                     G_CALLBACK(on_history_drag_begin), page);
    g_signal_connect(drag_mem, "drag-update",
                     G_CALLBACK(on_history_drag_update), page);
    g_signal_connect(drag_mem, "drag-end",
                     G_CALLBACK(on_history_drag_end), page);
    gtk_widget_add_controller(page->mem_area, GTK_EVENT_CONTROLLER(drag_mem));

    history_refresh(page);

    page->timer = g_timeout_add_seconds(30, on_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "history-timer",
                           GUINT_TO_POINTER(page->timer), remove_source);
    g_object_set_data_full(G_OBJECT(vbox), "history-page", page, g_free);

    return vbox;
}
