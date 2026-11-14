/*
 * gui_page_disk.h - 磁盘设备详情
 */

#ifndef FMON_GUI_PAGE_DISK_H
#define FMON_GUI_PAGE_DISK_H

#include <gtk/gtk.h>

#include "gui_chart.h"

/* 磁盘详情：放在页面标题行右侧，不再单独占一张卡片 */
GtkWidget *gui_disk_details_header(void);
void gui_disk_details_update(void);

/* 实时性能（IOPS / 延迟 / 繁忙 / 队列），放在磁盘标题行右上角 */
GtkWidget *gui_disk_perf_line(void);
void       gui_disk_perf_update(void);

/* SMART 健康度与累计字节（温度/寿命/备用空间） */
GtkWidget *gui_disk_smart_line(void);

/* 每块磁盘的速率/累计/繁忙明细 */
GtkWidget *gui_disk_table_box(void);
void       gui_disk_table_update(void);
void       gui_disk_smart_update(void);

/* 温度曲线：界面内环形缓冲，每秒采样一次 SMART 温度 */
CurveData *gui_disk_temp_curve(void);
void       gui_disk_temp_sample(void);

#endif /* FMON_GUI_PAGE_DISK_H */

