/*
 * gui_nav.h - 两级导航
 */

#ifndef FMON_GUI_NAV_H
#define FMON_GUI_NAV_H

#include <gtk/gtk.h>

/* 页面切换回调：参数是页面 id（如 "cpu" / "gpu1"） */
typedef void (*GuiNavSwitchFunc)(const char *page_id);

GtkWidget *gui_nav_sidebar_new(void);
GtkWidget *gui_nav_revealer_new(void);
GtkWidget *gui_nav_row_new(const char *title, const char *icon_name,
                           const char *page_name);

void gui_nav_set_switch(GuiNavSwitchFunc fn);
void gui_nav_build_groups(void);
void gui_nav_start(const char *page_id);

#endif /* FMON_GUI_NAV_H */
