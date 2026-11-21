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
#include <stddef.h>
#include <stdint.h>

#include "fmon_shm.h"

typedef struct AlertSettings AlertSettings;

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

    /* 可选的历史页局部视窗（微秒）。为 0 时使用全局实时窗口。 */
    uint64_t      view_start_us;
    uint64_t      view_end_us;

    /* 历史页固定范围：不叠加实时图全局窗口；长历史 provider 不设此位。 */
    gboolean      fixed_range;
    /* 外部数据源正在接管曲线：停止本地环形写。 */
    gboolean      frozen;

    /* 可选告警阈值：指向 GUI 当前 AlertSettings，图表每帧读取，修改后立即生效。 */
    const AlertSettings *alert_settings;
    int                  alert_metric; /* 0 CPU, 1 MEM, 2 SWAP, 6 POOL, 12 TEMP */
} CurveData;

void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data);

/* 读取当前告警阈值（曲线 alert_metric + CurveData.alert_settings）。
 * 返回 FALSE 表示该曲线没有启用阈值；label 为可直接绘制的短文本。 */
gboolean gui_chart_curve_threshold(const CurveData *cd, double *value,
                                   char *label, size_t label_len);

/* 全局实时图时间窗口（微秒）；0 表示完整 SHM 环形缓冲（20 分钟）。
 * 只裁剪显示范围，不修改原始采样；1 小时/24 小时请使用历史页。 */
uint64_t gui_chart_window_us(void);
void     gui_chart_set_window_us(uint64_t us);

/* 计算某条曲线在全局窗口内的可见时间范围。返回 FALSE 表示该曲线没有
 * 可用的逐点时间戳（此时调用方应退回完整/等间距范围）。 */
gboolean gui_chart_view_range(const CurveData *cd, uint32_t points,
                              uint64_t *t0, uint64_t *t1);

/* 交互：滚轮切档、左键拖动平移、双击回到最新。 */
void gui_chart_attach_interactions(GtkWidget *area, const CurveData *cd);
void gui_chart_set_view_end_us(uint64_t us);
void gui_chart_reset_view(void);
void gui_chart_draw_selection(cairo_t *cr, GtkWidget *area, double left,
                              double top, double w, double h);
void gui_chart_selection_set(GtkWidget *area, double x0, double x1,
                             gboolean active);

/* 时间窗口变化通知（用于同步顶部下拉框）。 */
typedef void (*GuiChartWindowNotify)(uint64_t us);
void gui_chart_set_window_notify(GuiChartWindowNotify cb);

/* 图表设置：百分比指标默认固定 0–100；打开自动量程后按窗口数据缩放。
 * 状态持久化在 $XDG_CONFIG_HOME/fmon/chart_auto_range。 */
void     gui_chart_settings_init(void);
gboolean gui_chart_auto_range(void);
void     gui_chart_set_auto_range(gboolean enabled);

#ifdef __cplusplus
}
#endif

#endif /* GUI_CHART_H */
