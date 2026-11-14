/*
 * gui_runtime.h - 运行时状态与顶部信息
 */

#ifndef FMON_GUI_RUNTIME_H
#define FMON_GUI_RUNTIME_H

#include <gtk/gtk.h>

#include "alerts.h"
#include "fmon_shm.h"

/* 运行时共享状态（GUI 内部） */
extern FmonShmData  g_snap;
extern FmonShmData *g_live;
extern gchar       *g_alert_summary;
extern GtkWidget   *g_header_sub;
extern GtkWidget   *g_banner;
extern GtkWidget   *g_banner_label;
extern GtkWidget   *g_alert_label;
extern guint        g_ui_timer;

void gui_runtime_bind(GtkWidget *header_sub, GtkWidget *banner,
                      GtkWidget *banner_label, GtkWidget *alert_label,
                      const AlertSettings *settings);

void gui_runtime_attach(void);
void gui_runtime_snapshot(void);
void gui_runtime_banner(void);
void gui_runtime_header(const char *title, const char *page_name);
void gui_runtime_alert_indicator(void);

void gui_runtime_start_timer(GSourceFunc fn);
void gui_runtime_stop_timer(void);
void gui_runtime_shutdown(void);

#endif /* FMON_GUI_RUNTIME_H */
