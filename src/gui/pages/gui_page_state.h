/*
 * gui_page_state.h - 页面状态对象（唯一持有者）
 */

#ifndef FMON_GUI_PAGE_STATE_H
#define FMON_GUI_PAGE_STATE_H

#include <gtk/gtk.h>

#include "gui_pages.h"
#include "combo_chart.h"
#include "gui_chart.h"

extern MetricPage g_metrics[METRIC_PAGE_COUNT];
extern ComboPage  g_net_page;
extern ComboPage  g_disk_page;
extern ComboChart g_net_chart;
extern ComboChart g_disk_chart;
extern CurveData  g_net_rx_cd;
extern CurveData  g_net_tx_cd;
extern CurveData  g_disk_rd_cd;
extern CurveData  g_disk_wr_cd;
extern GtkWidget *g_net_summary;
extern GtkWidget *g_disk_summary;
extern GtkWidget *g_mem_note;
extern ComboPage  g_mem_page;
extern ComboChart g_mem_chart;
extern ComboPage  g_zsum_page;
extern ComboPage  g_zbw_page;
extern ComboPage  g_cpu_page;
extern ComboPage  g_zpool_page;
extern CurveData  g_zpool_cd;
extern ComboChart g_zsum_chart;
extern ComboChart g_zbw_chart;
extern ComboChart g_cpu_chart;
extern GtkWidget *g_core_area;
extern GtkWidget *g_core_caption;

#endif /* FMON_GUI_PAGE_STATE_H */
