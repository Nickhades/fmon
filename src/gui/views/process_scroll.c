/*
 * process_scroll.c - 进程列表滚回顶部
 */

#include "process_scroll.h"

#include <gtk/gtk.h>

#include "process_internal.h"

static gboolean proc_scroll_top_cb(gpointer data)
{
    ProcPage *page = data;

    if (page->scroll != NULL && page->root != NULL &&
        gtk_widget_get_mapped(page->root))
        gtk_adjustment_set_value(
            gtk_scrolled_window_get_vadjustment(
                GTK_SCROLLED_WINDOW(page->scroll)),
            0.0);

    return G_SOURCE_REMOVE;
}

static void proc_scroll_top_destroy(gpointer data)
{
    ProcPage *page = data;

    if (page != NULL && page->root != NULL)
        g_object_unref(page->root);
}

void proc_view_scroll_top(ProcPage *page)
{
    if (page->scroll == NULL || page->root == NULL)
        return;

    /* 布局完成后 GtkListView 还会调整一次位置，延迟再补一次 */
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page->scroll)),
        0.0);
    g_object_ref(page->root);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 60, proc_scroll_top_cb, page,
                       proc_scroll_top_destroy);
}

/* 切回本页时立刻刷新一次：隐藏页面的定时器不工作（见 on_refresh_tick），
 * 若不补一次，切回来可能先看到最多 3 秒前的旧数据。 */
