/*
 * gui_pages.h - 指标页（metric page）与各指标 updater
 */

#ifndef FMON_GUI_PAGES_H
#define FMON_GUI_PAGES_H

#include <gtk/gtk.h>

#include "fmon_shm.h"
#include "gui_chart.h"

typedef struct MetricPage MetricPage;

struct MetricPage {
    GtkWidget *vals[12];
    GtkWidget *area;      /* 大曲线绘图区，定时重绘实现实时刷新 */
    GtkWidget *sub;       /* 副标题（可动态显示型号） */
    void     (*update)(MetricPage *p);
};

/* 组合页：标题 + 曲线 + 多行数值 */
typedef struct {
    GtkWidget *vals[12];
    GtkWidget *area;
    GtkWidget *sub;          /* 副标题（可动态显示型号） */
    GtkWidget *header_right; /* 标题行右侧的空位（如内存条摘要） */
    int        nvals;
} ComboPage;

#define METRIC_PAGE_COUNT 14

CurveData *gui_metric_curve(int idx);
void gui_metric_curves_sync(const FmonShmData *snap);
void gui_metric_curves_bind(const FmonShmData *snap);

void gui_upd_zpool(MetricPage *p);
void gui_upd_arc(MetricPage *p);
void gui_upd_zhit(MetricPage *p);
void gui_upd_zread(MetricPage *p);
void gui_upd_zwrite(MetricPage *p);
void gui_upd_zprune(MetricPage *p);
void gui_upd_temp(MetricPage *p);
void gui_upd_batt(MetricPage *p);

#endif /* FMON_GUI_PAGES_H */
