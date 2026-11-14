/*
 * gui_chart_util.c - 单曲线图的时间/坐标轴格式化与量程计算
 */

#include "gui_chart_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void gui_chart_format_time_label(char *buf, size_t len, double secs)
{
    if (secs <= 0.5) {
        snprintf(buf, len, "现在");
    } else if (secs >= 60.0) {
        double m = secs / 60.0;
        if (m >= 10.0 || m == (double)(int)m)
            snprintf(buf, len, "-%.0fm", m);
        else
            snprintf(buf, len, "-%.1fm", m);
    } else {
        snprintf(buf, len, "-%.0fs", secs);
    }
}

/* 把最大值向上取整到“好看”的刻度 */
double gui_chart_nice_max(double max_val)
{
    if (!(max_val > 0.0))
        return 10.0;

    max_val *= 1.10;

    double exp10 = 1.0;
    double temp  = max_val;

    while (temp >= 10.0) {
        temp /= 10.0;
        exp10 *= 10.0;
    }
    while (temp < 1.0) {
        temp *= 10.0;
        exp10 /= 10.0;
    }

    /* 向上取整到 1 / 2 / 2.5 / 5 / 10 档，避免取整后比数据还小、曲线被裁顶 */
    double nice_d = 10.0;

    if (temp <= 1.0)
        nice_d = 1.0;
    else if (temp <= 2.0)
        nice_d = 2.0;
    else if (temp <= 2.5)
        nice_d = 2.5;
    else if (temp <= 5.0)
        nice_d = 5.0;

    return nice_d * exp10;
}

void gui_chart_format_axis_label(char *buf, size_t len, double value,
                              const char *unit, double span)
{
    int dec = 0;

    if (span < 0.1)
        dec = 3;
    else if (span < 1.0)
        dec = 2;
    else if (span < 10.0)
        dec = 1;

    if (unit != NULL && strcmp(unit, "%") == 0) {
        snprintf(buf, len, "%.*f%%", dec, value);
        return;
    }

    if (unit != NULL && strcmp(unit, "\xc2\xb0""C") == 0) {
        snprintf(buf, len, "%.*f°C", dec, value);
        return;
    }

    if (value >= 1024.0 * 1024.0)
        snprintf(buf, len, "%.1fG", value / (1024.0 * 1024.0));
    else if (value >= 1024.0)
        snprintf(buf, len, "%.0fK", value / 1024.0);
    else if (value < 10.0)
        snprintf(buf, len, "%.1f", value);
    else
        snprintf(buf, len, "%.0f", value);
}


