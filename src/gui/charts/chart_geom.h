/*
 * chart_geom.h - 折线图几何/量程/路径（纯 C + Cairo，不依赖 GTK）
 *
 * 设计目标：
 *   1. 绘图端不做任何数据平滑、突变过滤或百分位截断；
 *   2. 坐标轴必须覆盖窗口内全部有效值，不能悄悄把峰值裁到 100%；
 *   3. 无效值（NaN/Inf）表示“没有采样”，折线必须断开，不能补 0；
 *   4. 点数多于像素列时按列保留首/末/最小/最大，尖峰不丢。
 *
 * 这些函数可以脱离 GTK 单测，也可以离屏渲染成 Cairo image surface。
 */

#ifndef FMON_CHART_GEOM_H
#define FMON_CHART_GEOM_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMON_CHART_MAX_TICKS 10

typedef struct {
    double lo;        /* 轴下限（含刻度对齐） */
    double hi;        /* 轴上限（含刻度对齐） */
    double step;      /* 相邻刻度间隔 */
    int    intervals; /* 间隔数，刻度数 = intervals + 1 */
} FmonChartScale;

typedef struct {
    double   x;
    double   y;
    bool     gap; /* true：本点之前断开，开始新的子路径 */
    uint32_t src; /* 对应的历史槽位序号（0 = 最旧），用于按时间重映射 x */
} FmonChartPoint;

/* 扫描环形历史中的有效值（忽略 NaN/Inf）。
 * 返回 false 表示窗口内没有有效采样。 */
bool fmon_chart_history_range(const double *history, uint32_t capacity,
                              uint32_t count, uint32_t index,
                              double *lo, double *hi);

/* 取环形历史中第 i 个有效槽位（0 = 最旧）。
 * 越界或非有限值返回 false。 */
bool fmon_chart_history_value(const double *history, uint32_t capacity,
                              uint32_t count, uint32_t index, uint32_t i,
                              double *out);

/* 取环形时间戳数组中的第 i 个点；0/越界返回 false。 */
bool fmon_chart_history_time(const uint64_t *time_us, uint32_t capacity,
                             uint32_t count, uint32_t index, uint32_t i,
                             uint64_t *out);

/* 扫描时间戳数组，返回最旧/最新的有效时间戳。
 * 少于两个有效时间戳时返回 false。 */
bool fmon_chart_time_range(const uint64_t *time_us, uint32_t capacity,
                           uint32_t count, uint32_t index, uint64_t *first,
                           uint64_t *last);

/* 把时间戳映射到 [x0, x0+width]；t1<=t0 时返回 x0。 */
double fmon_chart_time_x(uint64_t t, uint64_t t0, uint64_t t1, double x0,
                         double width);

/* 按真实时间重映射已生成折线点的 x。
 * gap_us > 0 时，相邻采样时间差超过该值即断开（用于 daemon 停摆）。
 * time_us 为 NULL 或 t1<=t0 时保持原有等间距 x。 */
void fmon_chart_retime(FmonChartPoint *pts, size_t n, const uint64_t *time_us,
                       uint32_t capacity, uint32_t count, uint32_t index,
                       double x0, double width, uint64_t t0, uint64_t t1,
                       uint64_t gap_us);

/* 计算“好看”的轴范围。
 * fixed_lo < 0 表示自动；fixed_hi <= 0 表示自动。
 * 固定值只是基准，数据超出时轴仍会扩展，保证不裁掉真实峰值。
 * min_span > 0 时给窄幅数据一个最小可视跨度（仅影响坐标轴，不改数据）。 */
bool fmon_chart_scale(double data_lo, double data_hi, double fixed_lo,
                      double fixed_hi, double min_span, int target_intervals,
                      FmonChartScale *out);

/* 把环形历史映射为屏幕折线点。
 * 返回点数；out 至少需要 count + 4 个元素。
 * max_columns 为 0 或 >= count 时逐点输出；否则按像素列做 min/max 抽取。 */
size_t fmon_chart_polyline(const double *history, uint32_t capacity,
                           uint32_t count, uint32_t index, double x0,
                           double width, double y0, double height, double lo,
                           double hi, uint32_t max_columns, FmonChartPoint *out,
                           size_t out_cap);

/* 同 fmon_chart_polyline，但按 time_us 映射真实时间轴。 */
size_t fmon_chart_polyline_at(const double *history, const uint64_t *time_us,
                              uint32_t capacity, uint32_t count,
                              uint32_t index, uint64_t t0, uint64_t t1,
                              double x0, double width, double y0, double height,
                              double lo, double hi, uint32_t max_columns,
                              uint64_t gap_us, FmonChartPoint *out,
                              size_t out_cap);

/* 把折线点画进当前 Cairo 路径（遇到 gap 自动 move_to）。 */
void fmon_chart_cairo_path(cairo_t *cr, const FmonChartPoint *pts, size_t n);

/* 构造面积填充路径：每个连续有效段单独闭合到 baseline，不跨缺口。 */
void fmon_chart_cairo_fill(cairo_t *cr, const FmonChartPoint *pts, size_t n,
                           double baseline);

/* 构造 min/max 包络带：upper 正向走、lower 反向走，形成闭合带。
 * 两个点集必须共享 x/gap 语义；任一侧出现 gap 即断开。 */
void fmon_chart_cairo_band(cairo_t *cr, const FmonChartPoint *lower,
                           const FmonChartPoint *upper, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FMON_CHART_GEOM_H */
