/*
 * gui_cpu_gauge.h - CPU 使用率仪表与健康摘要
 */

#ifndef FMON_GUI_CPU_GAUGE_H
#define FMON_GUI_CPU_GAUGE_H

#include <gtk/gtk.h>

GtkWidget *gui_cpu_gauge_card(void);
GtkWidget *gui_cpu_gauge_header_right(void);
GtkWidget *gui_cpu_gauge_body(void);
void       gui_cpu_gauge_update(void);

/* 共用的等宽字体设置（温度图也使用同一字号体系）。 */
void gui_cpu_gauge_font(cairo_t *cr, double size);

#endif
