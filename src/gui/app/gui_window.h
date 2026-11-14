/*
 * gui_window.h - 应用外壳入口
 */

#ifndef FMON_GUI_WINDOW_H
#define FMON_GUI_WINDOW_H

#include <gio/gio.h>
#include <gtk/gtk.h>

/* 处理 --page/--gpu/--theme/--version */
gint gui_window_handle_options(GApplication *app, GVariantDict *options,
                               gpointer user_data);

/* 创建主窗口、页面栈与刷新定时器 */
void gui_window_activate(GtkApplication *app, gpointer user_data);

/* 从其它页面跳转到指定 page id */
void fmon_gui_switch_page(const char *page_id);

#endif /* FMON_GUI_WINDOW_H */
