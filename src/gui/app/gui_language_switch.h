/*
 * gui_language_switch.h - 语言切换与重启
 */

#ifndef FMON_GUI_LANGUAGE_SWITCH_H
#define FMON_GUI_LANGUAGE_SWITCH_H

#include <gtk/gtk.h>

void gui_language_toggled(GtkToggleButton *btn, gpointer data);
void gui_language_set_syncing(gboolean syncing);

#endif
