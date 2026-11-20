/*
 * gui_chart_util.c - 单曲线图的时间/坐标轴/数值格式化
 *
 * 这里只做“显示格式”，不改数据。所有函数都不依赖 GLib/GTK，
 * 可以被 tests/test_chart.c 直接单测。
 */

#include "gui_chart_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int decimals_for_step(double step)
{
    if (!isfinite(step) || step <= 0.0)
        return 0;

    /* 按刻度间隔本身确定小数位：0.25 需要 2 位，2.5 需要 1 位，
     * 否则相邻刻度会被四舍五入成同一个字符串。 */
    for (int dec = 0; dec <= 6; dec++) {
        double scaled = step * pow(10.0, (double)dec);

        if (fabs(scaled - round(scaled)) < 1e-9)
            return dec;
    }

    return 3;
}

static void format_rate(char *buf, size_t len, double kbps)
{
    const double kb = 1024.0;

    if (!isfinite(kbps)) {
        snprintf(buf, len, "—");
        return;
    }

    if (kbps >= kb * kb)
        snprintf(buf, len, "%.2f GB/s", kbps / (kb * kb));
    else if (kbps >= kb)
        snprintf(buf, len, "%.2f MB/s", kbps / kb);
    else if (kbps >= 1.0)
        snprintf(buf, len, "%.1f KB/s", kbps);
    else
        snprintf(buf, len, "%.0f B/s", kbps * kb);
}

void gui_chart_format_time_label(char *buf, size_t len, double secs)
{
    if (!isfinite(secs) || secs <= 0.5) {
        snprintf(buf, len, "现在");
    } else if (secs >= 3600.0) {
        double h = secs / 3600.0;

        if (h >= 10.0 || h == (double)(int)h)
            snprintf(buf, len, "-%.0fh", h);
        else
            snprintf(buf, len, "-%.1fh", h);
    } else if (secs >= 60.0) {
        double m = secs / 60.0;

        if (m >= 10.0 || m == (double)(int)m)
            snprintf(buf, len, "-%.0fm", m);
        else
            snprintf(buf, len, "-%.1fm", m);
    } else if (secs >= 10.0 || secs == (double)(int)secs) {
        snprintf(buf, len, "-%.0fs", secs);
    } else {
        snprintf(buf, len, "-%.1fs", secs);
    }
}

void gui_chart_format_offset(char *buf, size_t len, double secs,
                             double interval_secs)
{
    if (!isfinite(secs) || secs <= (interval_secs > 0.0 ? interval_secs / 2.0
                                                       : 0.5)) {
        snprintf(buf, len, "现在");
    } else if (secs >= 3600.0) {
        double h = secs / 3600.0;

        snprintf(buf, len, (fabs(h - round(h)) < 0.05) ? "-%.0fh" : "-%.1fh",
                 h);
    } else if (secs >= 60.0) {
        double m = secs / 60.0;

        snprintf(buf, len, (fabs(m - round(m)) < 0.05) ? "-%.0fm" : "-%.1fm",
                 m);
    } else if (isfinite(interval_secs) && interval_secs > 0.0 &&
               interval_secs < 1.0) {
        snprintf(buf, len, "-%.2fs", secs);
    } else {
        snprintf(buf, len, "-%.0fs", secs);
    }
}

void gui_chart_format_axis_label(char *buf, size_t len, double value,
                                 const char *unit, double step)
{
    if (!isfinite(value)) {
        snprintf(buf, len, "—");
        return;
    }

    if (unit != NULL && strcmp(unit, "%") == 0) {
        snprintf(buf, len, "%.*f%%", decimals_for_step(step), value);
        return;
    }

    if (unit != NULL && strcmp(unit, "°C") == 0) {
        snprintf(buf, len, "%.*f°C", decimals_for_step(step), value);
        return;
    }

    if (unit != NULL && strcmp(unit, "KB/s") == 0) {
        format_rate(buf, len, value);
        return;
    }

    if (unit != NULL && strcmp(unit, "GB") == 0) {
        snprintf(buf, len, "%.*f GB", decimals_for_step(step), value);
        return;
    }

    if (unit != NULL && strcmp(unit, "次") == 0) {
        snprintf(buf, len, "%.0f", value);
        return;
    }

    if (unit != NULL && unit[0] != '\0')
        snprintf(buf, len, "%.*f%s", decimals_for_step(step), value, unit);
    else
        snprintf(buf, len, "%.*f", decimals_for_step(step), value);
}

void gui_chart_format_value(char *buf, size_t len, double value,
                            const char *unit)
{
    if (!isfinite(value)) {
        snprintf(buf, len, "—");
        return;
    }

    if (unit != NULL && strcmp(unit, "%") == 0) {
        snprintf(buf, len, "%.2f%%", value);
        return;
    }

    if (unit != NULL && strcmp(unit, "°C") == 0) {
        snprintf(buf, len, "%.1f°C", value);
        return;
    }

    if (unit != NULL && strcmp(unit, "KB/s") == 0) {
        format_rate(buf, len, value);
        return;
    }

    if (unit != NULL && strcmp(unit, "GB") == 0) {
        snprintf(buf, len, "%.2f GB", value);
        return;
    }

    if (unit != NULL && strcmp(unit, "次") == 0) {
        snprintf(buf, len, "%.0f 次", value);
        return;
    }

    if (unit != NULL && unit[0] != '\0')
        snprintf(buf, len, "%.2f %s", value, unit);
    else
        snprintf(buf, len, "%.2f", value);
}
