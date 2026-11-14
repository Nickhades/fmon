/*
 * process_view.h - 进程管理页面
 */

#ifndef PROCESS_VIEW_H
#define PROCESS_VIEW_H

#include <gtk/gtk.h>

GtkWidget *gui_process_view_new(void);

/* 切到进程页后按 PID 选中（供服务详情等页面跳转使用） */
gboolean gui_process_select_pid(guint32 pid);

#endif /* PROCESS_VIEW_H */
