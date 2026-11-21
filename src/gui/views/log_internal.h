/*
 * log_internal.h - 日志页内部状态
 */

#ifndef FMON_LOG_INTERNAL_H
#define FMON_LOG_INTERNAL_H

#include <gtk/gtk.h>

#define LOG_MAX_LINES 2000

typedef struct {
    GtkWidget *root;
    GtkWidget *text_view;
    GtkWidget *scroll;
    GtkWidget *search;
    GtkWidget *exclude;
    GtkWidget *regex_btn;
    GtkWidget *dedup_btn;
    GtkWidget *follow_btn;
    GtkWidget *path_label;
    GtkWidget *level_dd;
    GtkWidget *preset_dd;
    GtkWidget *range_dd;

    GPtrArray *sources;
    gchar    *path;
    gchar    *cmd;
    GString  *raw;
    GString  *pending;
    goffset   offset;
    gboolean  initialized;
    time_t    file_mtime;
    gboolean  append;
    gboolean  follow;
    gboolean  busy;
    guint     generation;
    guint     req_gen;
    GFileMonitor *monitor;
    GHashTable  *ctx_cache;
    guint     timer;
} LogPage;

#endif
