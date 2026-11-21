/*
 * chart_geom.c - 折线图几何/量程/路径实现
 *
 * 只依赖 libm 与 Cairo，不依赖 GTK，便于单元测试和离屏渲染测试。
 */

#include "chart_geom.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 1 / 2 / 2.5 / 5 / 10 × 10^n：监控面板常用的刻度档位。 */
static double nice_step(double raw)
{
    if (!isfinite(raw) || raw <= 0.0)
        return 1.0;

    double exp10 = pow(10.0, floor(log10(raw)));
    double f     = raw / exp10;
    double n;

    if (f <= 1.0)
        n = 1.0;
    else if (f <= 2.0)
        n = 2.0;
    else if (f <= 2.5)
        n = 2.5;
    else if (f <= 5.0)
        n = 5.0;
    else
        n = 10.0;

    double step = n * exp10;

    return (isfinite(step) && step > 0.0) ? step : 1.0;
}

static uint32_t ring_count(uint32_t capacity, uint32_t count)
{
    if (capacity == 0)
        return 0;
    return (count > capacity) ? capacity : count;
}

static uint32_t ring_start(uint32_t capacity, uint32_t count, uint32_t index)
{
    uint32_t n = ring_count(capacity, count);

    if (n == 0)
        return 0;
    return (index + capacity - n) % capacity;
}

bool fmon_chart_history_value(const double *history, uint32_t capacity,
                              uint32_t count, uint32_t index, uint32_t i,
                              double *out)
{
    uint32_t n = ring_count(capacity, count);

    if (history == NULL || i >= n)
        return false;

    double v = history[(ring_start(capacity, count, index) + i) % capacity];

    if (!isfinite(v))
        return false;

    if (out != NULL)
        *out = v;
    return true;
}

bool fmon_chart_history_time(const uint64_t *time_us, uint32_t capacity,
                             uint32_t count, uint32_t index, uint32_t i,
                             uint64_t *out)
{
    uint32_t n = ring_count(capacity, count);

    if (time_us == NULL || i >= n)
        return false;

    uint64_t t = time_us[(ring_start(capacity, count, index) + i) % capacity];

    if (t == 0)
        return false;

    if (out != NULL)
        *out = t;
    return true;
}

bool fmon_chart_time_range(const uint64_t *time_us, uint32_t capacity,
                           uint32_t count, uint32_t index, uint64_t *first,
                           uint64_t *last)
{
    uint32_t n = ring_count(capacity, count);

    if (time_us == NULL || n < 2)
        return false;

    uint64_t start = ring_start(capacity, count, index);
    uint64_t lo    = 0;
    uint64_t hi    = 0;
    bool     have  = false;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t t = time_us[(start + i) % capacity];

        if (t == 0)
            continue;
        if (!have) {
            lo = hi = t;
            have    = true;
        } else {
            if (t < lo)
                lo = t;
            if (t > hi)
                hi = t;
        }
    }

    if (!have || hi <= lo)
        return false;

    if (first != NULL)
        *first = lo;
    if (last != NULL)
        *last = hi;
    return true;
}

double fmon_chart_time_x(uint64_t t, uint64_t t0, uint64_t t1, double x0,
                         double width)
{
    if (t1 <= t0 || !isfinite(width) || width <= 0.0)
        return x0;

    double frac = (double)(t - t0) / (double)(t1 - t0);

    if (frac < 0.0)
        frac = 0.0;
    if (frac > 1.0)
        frac = 1.0;

    return x0 + width * frac;
}

void fmon_chart_retime(FmonChartPoint *pts, size_t n, const uint64_t *time_us,
                       uint32_t capacity, uint32_t count, uint32_t index,
                       double x0, double width, uint64_t t0, uint64_t t1,
                       uint64_t gap_us)
{
    if (pts == NULL || n == 0 || time_us == NULL || t1 <= t0)
        return;

    uint64_t prev          = 0;
    bool     have_prev     = false;
    bool     after_missing = false;

    for (size_t i = 0; i < n; i++) {
        uint64_t t;

        if (!fmon_chart_history_time(time_us, capacity, count, index,
                                     pts[i].src, &t)) {
            /* 时间戳缺失：保留等间距 x，但强制断线，避免假装连续。
             * 下一个有效点也要断开，否则会从缺失点连出一条假线段。 */
            pts[i].gap = true;
            after_missing = true;
            continue;
        }

        pts[i].x = fmon_chart_time_x(t, t0, t1, x0, width);

        if (after_missing) {
            pts[i].gap = true;
            after_missing = false;
        }

        if (have_prev && t < prev)
            pts[i].gap = true;
        if (have_prev && gap_us > 0 && t > prev && t - prev > gap_us)
            pts[i].gap = true;

        prev      = t;
        have_prev = true;
    }
}

bool fmon_chart_history_range(const double *history, uint32_t capacity,
                              uint32_t count, uint32_t index, double *lo,
                              double *hi)
{
    uint32_t n = ring_count(capacity, count);

    if (history == NULL || n == 0)
        return false;

    uint32_t start = ring_start(capacity, count, index);
    double   mn    = 0.0;
    double   mx    = 0.0;
    bool     have  = false;

    for (uint32_t i = 0; i < n; i++) {
        double v = history[(start + i) % capacity];

        if (!isfinite(v))
            continue;
        if (!have) {
            mn = mx = v;
            have    = true;
        } else {
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
    }

    if (!have)
        return false;

    if (lo != NULL)
        *lo = mn;
    if (hi != NULL)
        *hi = mx;
    return true;
}

/* 按范围扩展固定轴：固定值代表“希望的范围”，不是裁剪线。 */
static void include_fixed(double *lo, double *hi, double fixed_lo,
                          double fixed_hi, bool auto_lo, bool auto_hi,
                          double data_lo, double data_hi, bool have_data)
{
    (void)auto_hi;

    if (!auto_lo)
        *lo = fixed_lo;
    if (!auto_hi)
        *hi = fixed_hi;

    if (have_data) {
        if (data_lo < *lo)
            *lo = data_lo;
        if (data_hi > *hi)
            *hi = data_hi;
    }
}

bool fmon_chart_scale(double data_lo, double data_hi, double fixed_lo,
                      double fixed_hi, double min_span, int target_intervals,
                      FmonChartScale *out)
{
    if (out == NULL)
        return false;

    bool have_data = isfinite(data_lo) && isfinite(data_hi) &&
                     data_hi >= data_lo;
    bool auto_lo = !(isfinite(fixed_lo) && fixed_lo >= 0.0);
    bool auto_hi = !(isfinite(fixed_hi) && fixed_hi > 0.0);

    double lo;
    double hi;

    if (!have_data) {
        lo = auto_lo ? 0.0 : fixed_lo;
        hi = auto_hi ? 1.0 : fixed_hi;
    } else {
        lo = auto_lo ? data_lo : fixed_lo;
        hi = auto_hi ? data_hi : fixed_hi;
        include_fixed(&lo, &hi, fixed_lo, fixed_hi, auto_lo, auto_hi, data_lo,
                      data_hi, true);
    }

    if (!(hi > lo)) {
        double span = (fabs(lo) > 1.0) ? fabs(lo) * 0.10 : 1.0;

        if (auto_lo && auto_hi) {
            lo -= span / 2.0;
            hi += span / 2.0;
        } else if (auto_lo) {
            lo = hi - span;
        } else if (auto_hi) {
            hi = lo + span;
        } else {
            hi = lo + 1.0;
        }
    }

    if (isfinite(min_span) && min_span > 0.0 && hi - lo < min_span) {
        if (!auto_lo && auto_hi)
            hi = lo + min_span;
        else if (auto_lo && !auto_hi)
            lo = hi - min_span;
        else if (auto_lo && auto_hi) {
            double mid = (lo + hi) / 2.0;
            lo = mid - min_span / 2.0;
            hi = mid + min_span / 2.0;
        }
        /* 两端都固定时保持用户指定的范围；固定范围本身很窄时，
         * 放大显示是用户的选择，不能用 min_span 把它撑出边界。 */
    }

    /* 非负指标（使用率、速率）不要把轴拉到负数。 */
    if (auto_lo && have_data && data_lo >= 0.0 && lo < 0.0)
        lo = 0.0;

    if (!(hi > lo))
        hi = lo + 1.0;

    int target = (target_intervals < 1) ? 4 : target_intervals;
    if (target > FMON_CHART_MAX_TICKS)
        target = FMON_CHART_MAX_TICKS;

    /* 两端都固定时，刻度必须落在 [lo,hi] 内，不能为了“好看”
     * 把 0–0.5 撑成 0–0.6。先选一个接近的档位，再按整数等分。 */
    if (!auto_lo && !auto_hi) {
        double span = hi - lo;
        double st   = nice_step(span / (double)target);
        int    iv   = (int)lround(span / st);

        if (iv < 1)
            iv = 1;
        if (iv > FMON_CHART_MAX_TICKS)
            iv = FMON_CHART_MAX_TICKS;

        out->lo        = lo;
        out->hi        = hi;
        out->step      = span / (double)iv;
        out->intervals = iv;
        return have_data;
    }

    double step = nice_step((hi - lo) / (double)target);
    double slo  = floor((lo + step * 1e-9) / step) * step;
    double shi  = ceil((hi - step * 1e-9) / step) * step;

    if (!(shi > slo))
        shi = slo + step;

    int intervals = (int)lround((shi - slo) / step);

    if (intervals > FMON_CHART_MAX_TICKS) {
        step = nice_step((shi - slo) / (double)FMON_CHART_MAX_TICKS);
        slo  = floor((lo + step * 1e-9) / step) * step;
        shi  = ceil((hi - step * 1e-9) / step) * step;
        intervals = (int)lround((shi - slo) / step);
    }

    if (intervals < 1)
        intervals = 1;
    if (intervals > FMON_CHART_MAX_TICKS)
        intervals = FMON_CHART_MAX_TICKS;

    if (fabs(slo) < step * 1e-9)
        slo = 0.0;

    out->lo        = slo;
    out->hi        = shi;
    out->step      = step;
    out->intervals = intervals;

    return have_data;
}
