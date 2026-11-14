/*
 * gui_theme_ui.h - 主题分段开关（浅色 / 深色）
 */

#ifndef FMON_GUI_THEME_UI_H
#define FMON_GUI_THEME_UI_H

#include <gtk/gtk.h>

void       gui_theme_ui_init(const char *start_theme);

/* 两段式主题开关：☀ 浅色 / ☾ 深色 */
GtkWidget *gui_theme_ui_switch(void);

#endif /* FMON_GUI_THEME_UI_H */
