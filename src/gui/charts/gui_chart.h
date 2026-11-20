/*
 * gui_chart.h - 曲线绘制组件
 *
 * 与旧版的关键区别：绘图函数只依赖传入的 CurveData（指向界面自己的数据
 * 快照），不再读取全局共享内存指针。这样既避免了跨进程读撕裂，
 * 也让这个组件可以独立测试。
 */

#ifndef GUI_CHART_H
#define GUI_CHART_H

#include <gtk/gtk.h>
#include <stdint.h>

#include "fmon_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const double *history;   /* 长度至少 FMON_HISTORY_POINTS 的环形数组 */
    uint32_t      count;     /* 有效点数（0 表示还在采集） */
    uint32_t      index;     /* 环形缓冲区下一个写入位置 */
    double        max_val;   /* 期望上限；<= 0 自动。数据超出时轴会扩展，不裁峰值 */
    double        min_val;   /* 期望下限；< 0 自动（命中率等窄幅指标） */
    double        r, g, b;   /* 曲线颜色 */
    const char   *unit;      /* 坐标轴单位："%" / "KB/s" / "" */
    const char   *empty_hint;/* count < 2 时显示的提示；NULL 用默认文案 */
    gboolean      bars;      /* TRUE：按采样点画柱形（CPU 使用率） */

    /* 悬停读数 */
    gboolean      hovering;
    double        hover_x;      /* 鼠标在绘图区内的 x（像素） */
    uint64_t      interval_us;  /* 相邻点间隔（微秒）；无时间戳时使用 */
    uint64_t      end_time_us;  /* 最新点的时间戳（单调时钟，微秒） */

    /* 逐点时间戳（CLOCK_MONOTONIC，微秒），与 history 同环形成员索引。
     * 为 NULL 时退回 interval_us 等间距，兼容界面内自建的派生曲线。 */
    const uint64_t *time_us;

    /* 可选的最小/最大包络（与 history 同环形成员索引）。仅 24 小时历史
     * 页使用：history 是桶平均，包络保留每个时间桶的真实极值。 */
    const double *history_min;
    const double *history_max;
} CurveData;

void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data);

/* 图表设置：百分比指标默认固定 0–100；打开自动量程后按窗口数据缩放。
 * 状态持久化在 $XDG_CONFIG_HOME/fmon/chart_auto_range。 */
void     gui_chart_settings_init(void);
gboolean gui_chart_auto_range(void);
void     gui_chart_set_auto_range(gboolean enabled);

#ifdef __cplusplus
}
#endif

#endif /* GUI_CHART_H */


