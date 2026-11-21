/*
 * cpu_chart_util.h - CPU 曲线共用坐标轴
 */

#ifndef FMON_CPU_CHART_UTIL_H
#define FMON_CPU_CHART_UTIL_H

#include <gtk/gtk.h>

#include "gui_chart.h"

void cpu_chart_draw_axis(cairo_t *cr, const GdkRGBA *fg, double x, double y,
                         double w, double h, double maxv, int ticks,
                         int decimals, const char *suffix);
void cpu_chart_draw_time_axis(cairo_t *cr, const CurveData *cd, double x,
                              double y, double w, double h,
                              const GdkRGBA *fg);

#endif
