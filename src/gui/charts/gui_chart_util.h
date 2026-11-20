/*
 * gui_chart_util.h - 单曲线图的时间/坐标轴/数值格式化（纯 C）
 */

#ifndef FMON_GUI_CHART_UTIL_H
#define FMON_GUI_CHART_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 相对当前时刻的短标签：现在 / -30s / -1.5m / -2h */
void gui_chart_format_time_label(char *buf, size_t len, double secs);

/* 悬停读数用的相对时刻；interval_secs 用于决定秒级小数位。 */
void gui_chart_format_offset(char *buf, size_t len, double secs,
                             double interval_secs);

/* 轴标签。step 是刻度间隔，用它决定小数位，避免相邻刻度显示成同一个数。 */
void gui_chart_format_axis_label(char *buf, size_t len, double value,
                                 const char *unit, double step);

/* 悬停读数/峰值读数。rate 单位（KB/s）按 1024 进位显示。 */
void gui_chart_format_value(char *buf, size_t len, double value,
                            const char *unit);

#ifdef __cplusplus
}
#endif

#endif /* FMON_GUI_CHART_UTIL_H */
