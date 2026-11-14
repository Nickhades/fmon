/*
 * gui_page_net.h - 网络接口详情
 */

#ifndef FMON_GUI_PAGE_NET_H
#define FMON_GUI_PAGE_NET_H

#include <gtk/gtk.h>

GtkWidget *gui_net_details_card(void);
void gui_net_details_update(void);

/* 链路错误/丢包 + TCP 健康度（并入网络大卡片，无新卡片） */
GtkWidget *gui_net_health_box(void);
void       gui_net_health_update(void);

/* 网卡硬件型号（放在网络概览标题行右侧） */
GtkWidget *gui_net_nic_header(void);
void       gui_net_nic_update(void);

#endif /* FMON_GUI_PAGE_NET_H */
