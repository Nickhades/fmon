/*
 * gui_cpu_detail.h - CPU 时间分布 / 硬件型号 / 调度状态
 */

#ifndef FMON_GUI_CPU_DETAIL_H
#define FMON_GUI_CPU_DETAIL_H

#include <gtk/gtk.h>

GtkWidget *gui_cpu_hw_header(void);
void       gui_cpu_hw_update(void);
GtkWidget *gui_cpu_detail_box(void);
GtkWidget *gui_cpu_status_box(void);
void       gui_cpu_detail_update(void);

#endif
