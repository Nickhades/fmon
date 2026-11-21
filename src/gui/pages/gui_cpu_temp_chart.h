/*
 * gui_cpu_temp_chart.h - CPU 温度曲线与温度计
 */

#ifndef FMON_GUI_CPU_TEMP_CHART_H
#define FMON_GUI_CPU_TEMP_CHART_H

#include <gtk/gtk.h>

void gui_cpu_temp_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer data);

#endif
