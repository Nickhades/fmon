/*
 * gui_window_state.h - 主窗口大小持久化 / X11 居中
 */

#ifndef FMON_GUI_WINDOW_STATE_H
#define FMON_GUI_WINDOW_STATE_H

#include <gtk/gtk.h>

/* 在创建主窗口前加载尺寸配置。 */
void gui_window_state_init(void);

/* 绑定主窗口：设置默认大小/最小值，连接尺寸变化。 */
void gui_window_state_attach(GtkWindow *window);

/* 主窗口首次 present 后调用：安排 X11 居中校正。 */
void gui_window_state_center_after_present(void);

/* 关闭 / 退出 / 语言重启前落盘。 */
void gui_window_state_save(void);

#endif
