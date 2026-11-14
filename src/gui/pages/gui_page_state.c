/*
 * gui_page_state.c - 页面状态对象定义
 *
 * 这些对象由构建器创建、刷新器更新；集中定义便于模块间共享。
 */

#include "gui_page_state.h"

#include "gui_pages.h"
#include "combo_chart.h"

MetricPage g_metrics[METRIC_PAGE_COUNT];
ComboPage  g_net_page;
ComboPage  g_disk_page;
ComboChart g_net_chart;
ComboChart g_disk_chart;
CurveData  g_net_rx_cd;
CurveData  g_net_tx_cd;
CurveData  g_disk_rd_cd;
CurveData  g_disk_wr_cd;
GtkWidget *g_net_summary = NULL;
GtkWidget *g_disk_summary = NULL;
GtkWidget *g_mem_note = NULL;
ComboPage  g_mem_page;
ComboChart g_mem_chart;
ComboPage  g_zsum_page;
ComboPage  g_zbw_page;
ComboPage  g_cpu_page;
ComboPage  g_zpool_page;
CurveData  g_zpool_cd;
ComboChart g_zsum_chart;
ComboChart g_zbw_chart;
ComboChart g_cpu_chart;
GtkWidget *g_core_area    = NULL;
GtkWidget *g_core_caption = NULL;
