/*
 * disk_history_internal.h - 磁盘历史内部接口
 */

#ifndef FMON_DISK_HISTORY_INTERNAL_H
#define FMON_DISK_HISTORY_INTERNAL_H

#include <gtk/gtk.h>

typedef struct {
    double t;
    double v;
} DiskHistoryPoint;

typedef struct {
    char       dev[16];
    GtkWidget *area;
    GtkWidget *drop;
    GtkWidget *devdrop;
    GtkWidget *stats;

    GArray    *pts;
    double     ml, mt, pw, ph, since, window, vmin, vmax;
    double     hover_x;
    gboolean   hover;
} HistDlg;

typedef struct {
    char dev[16];
    int  metric;
} ExportCtx;

GArray    *disk_history_query_points(const char *dev, int metric);
GPtrArray *disk_history_query_devices(void);
void disk_history_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer data);

#endif
