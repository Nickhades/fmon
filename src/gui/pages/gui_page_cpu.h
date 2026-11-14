/*
 * gui_page_cpu.h - CPU 页的时间分布 / 每核状态 / 调度压力
 */

#ifndef FMON_GUI_PAGE_CPU_H
#define FMON_GUI_PAGE_CPU_H

#include <gtk/gtk.h>

GtkWidget *gui_cpu_detail_box(void);
void       gui_cpu_detail_update(void);

/* CPU 型号：放在标题行右上角，与其它页硬件型号同一字号 */
GtkWidget *gui_cpu_hw_header(void);
void       gui_cpu_hw_update(void);

/* 独立的半圆仪表卡片：当前使用率 / 历史平均 / 1-5-15 负载 */
GtkWidget *gui_cpu_gauge_card(void);
GtkWidget *gui_cpu_gauge_header_right(void);
GtkWidget *gui_cpu_gauge_body(void);

/* 状态与调度块（放在仪表卡片右侧） */
GtkWidget *gui_cpu_status_box(void);
void       gui_cpu_gauge_update(void);

/* 负载与温度卡片：左阶梯负载图 / 右阶梯温度图 + 体温表 */
GtkWidget *gui_cpu_thermal_card(void);
void       gui_cpu_thermal_update(void);

#endif /* FMON_GUI_PAGE_CPU_H */


