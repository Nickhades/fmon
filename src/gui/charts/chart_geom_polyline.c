/*
 * chart_geom_polyline.c - 采样抽取与折线点生成
 */

#include "chart_geom.h"

#include <math.h>

static uint32_t ring_count(uint32_t capacity, uint32_t count)
{
    return (count < capacity) ? count : capacity;
}

static uint32_t ring_start(uint32_t capacity, uint32_t count, uint32_t index)
{
    uint32_t n = ring_count(capacity, count);

    if (n == 0 || capacity == 0)
        return 0;

    return (index + capacity - n) % capacity;
}



static double map_y(double v, double lo, double hi, double y0, double height)
{
    if (!(hi > lo))
        return y0 + height;

    return y0 + height - ((v - lo) / (hi - lo)) * height;
}

/* 对一段连续有效数据，按原始顺序输出 first / min / max / last（去重）。 */
static size_t emit_run(const double *history, uint32_t capacity, uint32_t n,
                       uint32_t start, uint32_t i0, uint32_t i1, double x0,
                       double width, double y0, double height, double lo,
                       double hi, bool gap, FmonChartPoint *out, size_t out_cap,
                       size_t out_n)
{
    uint32_t idx[4];
    uint32_t cnt = 0;

    uint32_t first = i0;
    uint32_t last  = i1 - 1;
    uint32_t mn_i  = i0;
    uint32_t mx_i  = i0;
    double   mn    = history[(start + i0) % capacity];
    double   mx    = mn;

    for (uint32_t i = i0 + 1; i < i1; i++) {
        double v = history[(start + i) % capacity];

        if (!isfinite(v))
            continue;
        if (v < mn) {
            mn   = v;
            mn_i = i;
        }
        if (v > mx) {
            mx   = v;
            mx_i = i;
        }
    }

    idx[cnt++] = first;
    idx[cnt++] = mn_i;
    idx[cnt++] = mx_i;
    idx[cnt++] = last;

    /* 插入排序 + 去重，保持时间顺序；4 个元素，成本可忽略。 */
    for (uint32_t a = 1; a < cnt; a++) {
        uint32_t key = idx[a];
        int      b   = (int)a - 1;

        while (b >= 0 && idx[b] > key) {
            idx[b + 1] = idx[b];
            b--;
        }
        idx[b + 1] = key;
    }

    uint32_t prev = UINT32_MAX;
    bool     first_emitted = true;

    for (uint32_t a = 0; a < cnt; a++) {
        uint32_t i = idx[a];

        if (i == prev)
            continue;
        prev = i;

        if (out_n >= out_cap)
            break;

        double v = history[(start + i) % capacity];
        double x = (n > 1)
                       ? x0 + width * (double)i / (double)(n - 1)
                       : x0;

        out[out_n].x   = x;
        out[out_n].y   = map_y(v, lo, hi, y0, height);
        out[out_n].gap = gap && first_emitted;
        out[out_n].src = i;
        out_n++;
        first_emitted = false;
    }

    return out_n;
}

size_t fmon_chart_polyline(const double *history, uint32_t capacity,
                           uint32_t count, uint32_t index, double x0,
                           double width, double y0, double height, double lo,
                           double hi, uint32_t max_columns, FmonChartPoint *out,
                           size_t out_cap)
{
    uint32_t n = ring_count(capacity, count);

    if (history == NULL || out == NULL || out_cap == 0 || n == 0)
        return 0;

    uint32_t start = ring_start(capacity, count, index);

    if (max_columns == 0 || max_columns >= n) {
        size_t   out_n = 0;
        bool     gap   = true;

        for (uint32_t i = 0; i < n && out_n < out_cap; i++) {
            double v = history[(start + i) % capacity];

            if (!isfinite(v)) {
                gap = true;
                continue;
            }

            out[out_n].x   = (n > 1) ? x0 + width * (double)i / (double)(n - 1)
                                     : x0;
            out[out_n].y   = map_y(v, lo, hi, y0, height);
            out[out_n].gap = gap;
            out[out_n].src = i;
            out_n++;
            gap = false;
        }

        return out_n;
    }

    size_t   out_n = 0;
    bool     gap   = true;

    for (uint32_t b = 0; b < max_columns && out_n < out_cap; b++) {
        uint32_t i0 = (uint32_t)((uint64_t)n * b / max_columns);
        uint32_t i1 = (uint32_t)((uint64_t)n * (b + 1) / max_columns);

        if (i1 <= i0)
            i1 = i0 + 1;
        if (i1 > n)
            i1 = n;

        uint32_t i = i0;

        while (i < i1 && out_n < out_cap) {
            if (!isfinite(history[(start + i) % capacity])) {
                gap = true;
                i++;
                continue;
            }

            uint32_t run_start = i;

            while (i < i1 && isfinite(history[(start + i) % capacity]))
                i++;

            size_t before = out_n;

            out_n = emit_run(history, capacity, n, start, run_start, i, x0,
                             width, y0, height, lo, hi, gap, out, out_cap,
                             out_n);

            if (out_n > before)
                gap = false;
        }
    }

    return out_n;
}

size_t fmon_chart_polyline_at(const double *history, const uint64_t *time_us,
                              uint32_t capacity, uint32_t count,
                              uint32_t index, uint64_t t0, uint64_t t1,
                              double x0, double width, double y0, double height,
                              double lo, double hi, uint32_t max_columns,
                              uint64_t gap_us, FmonChartPoint *out,
                              size_t out_cap)
{
    size_t n = fmon_chart_polyline(history, capacity, count, index, x0, width,
                                   y0, height, lo, hi, max_columns, out,
                                   out_cap);

    if (n > 0)
        fmon_chart_retime(out, n, time_us, capacity, count, index, x0, width,
                          t0, t1, gap_us);

    return n;
}
