/*
 * gui_zfs_arc.h - ZFS ARC 明细与构成趋势
 */

#ifndef FMON_GUI_ZFS_ARC_H
#define FMON_GUI_ZFS_ARC_H

#include <gtk/gtk.h>

GtkWidget *gui_arc_detail_box(void);
void       gui_arc_detail_update(void);
GtkWidget *gui_arc_trend_chart(void);

#endif
