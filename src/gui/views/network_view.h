/*
 * network_view.h - 网络页面（连接 + 接口）
 */

#ifndef NETWORK_VIEW_H
#define NETWORK_VIEW_H

#include <gtk/gtk.h>

GtkWidget *gui_network_conn_view_new(void);   /* 活动连接 */
GtkWidget *gui_network_iface_view_new(void);  /* 网络接口 */

#endif /* NETWORK_VIEW_H */
