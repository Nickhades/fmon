/*
 * combo_chart.h - 多曲线合并图表
 *
 * 把多条波动不大的曲线画在同一个坐标系里（颜色区分），
 * 悬停时在右侧显示同一时刻各条曲线的数值。
 */

#ifndef COMBO_CHART_H
#define COMBO_CHART_H

#include <gtk/gtk.h>

#include "gui_chart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    CurveData  *cd;
    /* 可选的第二行说明（如“累计 32 KB · 操作 1”），画在带名下方，
     * 只有文字、没有底色，不影响曲线 */
    char        extra[160];
} ComboSeries;

typedef struct {
    ComboSeries series[4];
    int         n;
    double      hover_x;
    double      hover_y;
    gboolean    hovering;
} ComboChart;

void combo_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer data);
void combo_chart_motion(GtkEventControllerMotion *ctrl, double x, double y,
                        gpointer data);
void combo_chart_leave(GtkEventControllerMotion *ctrl, gpointer data);

#ifdef __cplusplus
}
#endif

#endif /* COMBO_CHART_H */
