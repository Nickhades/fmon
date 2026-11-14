/*
 * gui_page_gpu.h - 显卡独立页面
 */

#ifndef FMON_GUI_PAGE_GPU_H
#define FMON_GUI_PAGE_GPU_H

#include <gtk/gtk.h>

/* 创建第 slot 张显卡的页面 */
GtkWidget *gui_gpu_page_new(int slot);

/* 每秒刷新所有已存在显卡页的标题/曲线/详情 */
void gui_gpu_pages_update(void);

/* 把第 slot 张显卡的详情写入给定网格（会自动清空） */
void gui_gpu_fill_details(GtkWidget *grid, int slot);

#endif /* FMON_GUI_PAGE_GPU_H */
