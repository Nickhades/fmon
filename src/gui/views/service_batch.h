/*
 * service_batch.h - 服务批量启停
 */

#ifndef FMON_SERVICE_BATCH_H
#define FMON_SERVICE_BATCH_H

#include <gtk/gtk.h>

void svc_actions_batch_start(GtkButton *btn, gpointer data);
void svc_actions_batch_stop(GtkButton *btn, gpointer data);

#endif
