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

/* 记录启动时的可执行文件路径；语言切换 exec 自身时使用。 */
void fmon_gui_set_executable_path(const char *argv0);

/* 创建主窗口、页面栈与刷新定时器 */
void gui_window_activate(GtkApplication *app, gpointer user_data);

/* 从其它页面跳转到指定 page id */
void fmon_gui_switch_page(const char *page_id);

/* 当前可见页面 id；无窗口时返回 NULL。 */
const char *gui_window_current_page(void);

/* 语言已写入配置；exec 自身使新语言完整生效（避免旧控件中英混排）。 */
void fmon_gui_restart_for_language(void);

#endif /* FMON_GUI_WINDOW_H */
