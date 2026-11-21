/*
 * history_internal.h - 历史页内部数据结构与数据层接口
 */

#ifndef FMON_HISTORY_INTERNAL_H
#define FMON_HISTORY_INTERNAL_H

#include <gtk/gtk.h>

#include "gui_chart.h"

#define HIST_POINTS FMON_HISTORY_POINTS

typedef struct {
    double  *cpu_avg;
    double  *cpu_min;
    double  *cpu_max;
    double  *mem_avg;
    double  *mem_min;
    double  *mem_max;
    uint64_t *time_us;
    guint    cap;
} HistoryOut;

typedef struct {
    GtkWidget *root;
    GtkWidget *range_dd;
    GtkWidget *summary;
    GtkWidget *cpu_area;
    GtkWidget *mem_area;
    CurveData  cpu_cd;
    CurveData  mem_cd;
    guint      timer;
    guint      n;
    guint      interval_s;
    uint64_t   view_start_us;
    uint64_t   view_end_us;
    gboolean   custom;
    double     drag_x0;
    gint64     last_press_us;
} HistoryPage;

extern double   g_cpu[HIST_POINTS];
extern double   g_cpu_min[HIST_POINTS];
extern double   g_cpu_max[HIST_POINTS];
extern double   g_mem[HIST_POINTS];
extern double   g_mem_min[HIST_POINTS];
extern double   g_mem_max[HIST_POINTS];
extern uint64_t g_time_us[HIST_POINTS];

void history_refresh(HistoryPage *page);
void history_export_clicked(GtkButton *btn, gpointer data);

#endif
