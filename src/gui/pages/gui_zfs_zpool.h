/*
 * gui_zfs_zpool.h - ZFS 池摘要、详情、vdev、错误趋势
 */

#ifndef FMON_GUI_ZFS_ZPOOL_H
#define FMON_GUI_ZFS_ZPOOL_H

#include <gtk/gtk.h>

#include "gui_chart.h"

GtkWidget *gui_zpool_header_box(void);
GtkWidget *gui_zpool_detail_box(void);
void       gui_zpool_detail_update(void);
CurveData *gui_zpool_err_curve(void);

#endif
