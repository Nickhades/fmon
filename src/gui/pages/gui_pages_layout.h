/*
 * gui_pages_layout.h - 页面创建与栈装配
 */

#ifndef FMON_GUI_PAGES_LAYOUT_H
#define FMON_GUI_PAGES_LAYOUT_H

#include <gtk/gtk.h>

/* 创建所有页面并装配到新的 GtkStack；content 用于挂载该栈 */
GtkStack *gui_pages_layout_build(GtkWidget *content);

#endif /* FMON_GUI_PAGES_LAYOUT_H */
