/*
 * gui_coreview.h - CPU 每线程使用率（文字列表）
 */

#ifndef FMON_GUI_COREVIEW_H
#define FMON_GUI_COREVIEW_H

#include <gtk/gtk.h>

/* 创建每线程使用率列表（4 列，彩色竖条 + CPU N 总使用率 + 百分比） */
GtkWidget *gui_core_view_new(void);

/* 刷新标题与每个线程的百分比 */
void gui_core_view_update(GtkWidget *area, GtkWidget *caption);

#endif /* FMON_GUI_COREVIEW_H */
