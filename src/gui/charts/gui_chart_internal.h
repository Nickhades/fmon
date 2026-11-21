/*
 * gui_chart_internal.h - 折线图内部共享状态
 */

#ifndef FMON_GUI_CHART_INTERNAL_H
#define FMON_GUI_CHART_INTERNAL_H

#include <gtk/gtk.h>

extern uint64_t  g_chart_view_end_us;
extern uint64_t  g_chart_window_us;
extern GtkWidget *g_sel_area;
extern double     g_sel_x0;
extern double     g_sel_x1;
extern gboolean   g_sel_active;

uint64_t curve_latest_us(const CurveData *cd);

#endif
