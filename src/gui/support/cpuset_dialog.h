/*
 * cpuset_dialog.h - 进程 CPU 亲和性（cpuset）对话框
 */

#ifndef CPUSET_DIALOG_H
#define CPUSET_DIALOG_H

#include <gtk/gtk.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 显示某个进程的 CPU 绑定对话框 */
void fmon_cpuset_dialog_show(GtkWindow *parent, guint32 pid,
                             const gchar *comm, uid_t uid);

#ifdef __cplusplus
}
#endif

#endif /* CPUSET_DIALOG_H */
