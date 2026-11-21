/*
 * gui_page_cpu.h - CPU 页的时间分布 / 每核状态 / 调度压力
 */

#ifndef FMON_GUI_PAGE_CPU_H
#define FMON_GUI_PAGE_CPU_H

#include <gtk/gtk.h>

#include "gui_cpu_detail.h"

#include "gui_cpu_gauge.h"

/* 状态与调度块（放在仪表卡片右侧） */
GtkWidget *gui_cpu_status_box(void);
void       gui_cpu_gauge_update(void);

/* 负载与温度卡片：左折线平均负载图 / 右温度折线图 + 温度计 */
GtkWidget *gui_cpu_thermal_card(void);
void       gui_cpu_thermal_update(void);

#endif /* FMON_GUI_PAGE_CPU_H */
