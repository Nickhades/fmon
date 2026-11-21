/*
 * network_internal.h - 网络视图内部条目与列接口
 */

#ifndef FMON_NETWORK_INTERNAL_H
#define FMON_NETWORK_INTERNAL_H

#include <gtk/gtk.h>

typedef struct {
    GObject parent_instance;

    gchar   *user;
    gchar   *command;
    gchar   *proto;
    gchar   *local;
    gchar   *foreign;
    gchar   *state;
    gchar   *service;
    gchar   *iface;
    gint64   since_us;
    guint32  uid;
    guint32  fd;
    guint32  pid;
} NetItem;

typedef struct {
    GObject parent_instance;

    gchar   *name;
    gchar   *addr;
    gchar   *state;
    guint32  mtu;
    gdouble  rx;
    gdouble  tx;
    guint64  errors;
} IfaceItem;

GType net_item_get_type(void);
GType iface_item_get_type(void);

void network_conn_columns_attach(GtkColumnView *view);
void network_iface_columns_attach(GtkColumnView *view);

#endif
