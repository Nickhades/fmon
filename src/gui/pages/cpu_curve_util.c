/*
 * cpu_curve_util.c - CPU 曲线窗口统计（纯 C + GLib/Cairo 数据结构）
 */

#include "cpu_curve_util.h"

#include "chart_geom.h"

#include <math.h>

double cpu_curve_hist_at(const CurveData *cd, uint32_t i)
{
    double v;

    if (cd == NULL ||
        !fmon_chart_history_value(cd->history, FMON_HISTORY_POINTS,
                                  cd->count, cd->index, i, &v))
        return NAN;

    return v;
}

double cpu_curve_avg_window_value(const CurveData *cd, uint32_t win)
{
    if (cd == NULL || cd->count == 0)
        return NAN;

    uint32_t count = (cd->count > FMON_HISTORY_POINTS) ? FMON_HISTORY_POINTS
                                                       : cd->count;
    uint32_t i = count - 1;
    uint32_t from = (i + 1 > win) ? i + 1 - win : 0;
    uint32_t warm = (count > 20) ? 15 : 0;

    if (from < warm)
        from = warm;

    double   acc = 0.0;
    uint32_t n = 0;

    for (uint32_t k = from; k <= i; k++) {
        double v = cpu_curve_hist_at(cd, k);

        if (isfinite(v)) {
            acc += v;
            n++;
        }
    }

    return (n > 0) ? acc / (double)n : NAN;
}

double cpu_curve_avg_window_max(const CurveData *cd, uint32_t win)
{
    if (cd == NULL || cd->count == 0)
        return 0.0;

    uint32_t count = (cd->count > FMON_HISTORY_POINTS) ? FMON_HISTORY_POINTS
                                                       : cd->count;
    static double prefix[FMON_HISTORY_POINTS + 1];
    static uint32_t counts[FMON_HISTORY_POINTS + 1];

    prefix[0] = 0.0;
    counts[0] = 0;

    for (uint32_t i = 0; i < count; i++) {
        double v = cpu_curve_hist_at(cd, i);

        prefix[i + 1] = prefix[i] + (isfinite(v) ? v : 0.0);
        counts[i + 1] = counts[i] + (isfinite(v) ? 1u : 0u);
    }

    uint32_t i0 = (count > 20) ? 15 : 0;
    double   mx = 0.0;

    for (uint32_t i = i0; i < count; i++) {
        uint32_t from = (i + 1 > win) ? i + 1 - win : 0;

        if (from < i0)
            from = i0;

        uint32_t cnt = counts[i + 1] - counts[from];

        if (cnt == 0)
            continue;

        double v = (prefix[i + 1] - prefix[from]) / (double)cnt;

        if (v > mx)
            mx = v;
    }

    return mx;
}

double cpu_curve_nice_max(double v)
{
    if (!isfinite(v) || v <= 0.0)
        return 10.0;

    double m = ceil(v / 5.0) * 5.0;

    return (m < 10.0) ? 10.0 : m;
}
