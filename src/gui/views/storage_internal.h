/*
 * storage_internal.h - 存储条目与列
 */

#ifndef FMON_STORAGE_INTERNAL_H
#define FMON_STORAGE_INTERNAL_H

#include <gtk/gtk.h>

typedef struct {
    GObject parent_instance;

    gchar   *mountpoint;
    gchar   *device;
    gchar   *fstype;
    guint64  total;
    guint64  freeb;
    gdouble  pct;
    gdouble  inode_pct;
    gboolean ro;
} MountItem;

typedef struct {
    GObjectClass parent_class;
} MountItemClass;

GType mount_item_get_type(void);
void  storage_columns_attach(GtkColumnView *view);

#endif
