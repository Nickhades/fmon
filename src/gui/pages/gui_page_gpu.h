/*
 * gui_page_gpu.h - 显卡独立页面
 */

#ifndef FMON_GUI_PAGE_GPU_H
#define FMON_GUI_PAGE_GPU_H

#include <gtk/gtk.h>

#include "gui_chart.h"

/* 创建第 slot 张显卡的页面 */
GtkWidget *gui_gpu_page_new(int slot);

/* 每秒刷新所有已存在显卡页的标题/曲线/详情 */
void gui_gpu_pages_update(void);

/* 第 slot 张显卡的利用率曲线（供长历史 provider 接管）。 */
CurveData *gui_gpu_curve(int slot);
CurveData *gui_gpu_temp_curve(int slot);
CurveData *gui_gpu_mem_curve(int slot);

/* 把第 slot 张显卡的详情写入给定网格（会自动清空） */
void gui_gpu_fill_details(GtkWidget *grid, int slot);

#endif /* FMON_GUI_PAGE_GPU_H */
