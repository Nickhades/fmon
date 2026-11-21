/*
 * cpu_curve_util.h - CPU 曲线窗口统计
 */

#ifndef FMON_CPU_CURVE_UTIL_H
#define FMON_CPU_CURVE_UTIL_H

#include <stdint.h>

#include "gui_chart.h"

double cpu_curve_hist_at(const CurveData *cd, uint32_t i);
double cpu_curve_avg_window_value(const CurveData *cd, uint32_t win);
double cpu_curve_avg_window_max(const CurveData *cd, uint32_t win);
double cpu_curve_nice_max(double v);

#endif
