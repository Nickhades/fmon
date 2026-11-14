/*
 * gui_chart_util.h - 单曲线图辅助函数
 */

#ifndef FMON_GUI_CHART_UTIL_H
#define FMON_GUI_CHART_UTIL_H

#include <stddef.h>

void   gui_chart_format_time_label(char *buf, size_t len, double secs);
double gui_chart_nice_max(double max_val);
void   gui_chart_format_axis_label(char *buf, size_t len, double value,
                                   const char *unit, double span);

#endif /* FMON_GUI_CHART_UTIL_H */

