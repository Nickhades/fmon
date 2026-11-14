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
    double        max_val;   /* Y 轴固定最大值；<= 0 表示按数据自动量程 */
    double        min_val;   /* 固定下限；< 0 表示按窗口最小值自动缩放（命中率等） */
    double        r, g, b;   /* 曲线颜色 */
    const char   *unit;      /* 坐标轴单位："%" / "KB/s" / "" */
    const char   *empty_hint;/* count < 2 时显示的提示；NULL 用默认文案 */
    gboolean      bars;      /* TRUE：按采样点画柱形（CPU 使用率） */

    /* 悬停读数 */
    gboolean      hovering;
    double        hover_x;      /* 鼠标在绘图区内的 x（像素） */
    uint64_t      interval_us;  /* 相邻点间隔（微秒） */
    uint64_t      end_time_us;  /* 最新点的时间戳（单调时钟，微秒） */
} CurveData;

void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data);

#ifdef __cplusplus
}
#endif

#endif /* GUI_CHART_H */


