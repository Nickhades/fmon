/*
 * log_context.h - 日志上下文窗口与 PID 跳转
 */

#ifndef FMON_LOG_CONTEXT_H
#define FMON_LOG_CONTEXT_H

#include <gtk/gtk.h>

void log_context_pid_click(GtkGestureClick *gesture, int n_press, double x,
                           double y, gpointer data);

#endif
