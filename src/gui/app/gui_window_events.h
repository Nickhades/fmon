/*
 * gui_window_events.h - 窗口事件
 */

#ifndef FMON_GUI_WINDOW_EVENTS_H
#define FMON_GUI_WINDOW_EVENTS_H

#include <gtk/gtk.h>

gboolean gui_window_quit_signal(gpointer user_data);
gboolean gui_window_close_request(GtkWindow *window, gpointer data);
gboolean gui_window_key_pressed(GtkEventControllerKey *controller,
                                guint keyval, guint keycode,
                                GdkModifierType state, gpointer user_data);

#endif /* FMON_GUI_WINDOW_EVENTS_H */
