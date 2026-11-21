/*
 * gui_page_zfs.h - ZFS 池页：右上摘要 + 详情（性能/健康/vdev）
 */

#ifndef FMON_GUI_PAGE_ZFS_H
#define FMON_GUI_PAGE_ZFS_H

#include <gtk/gtk.h>

#include "gui_chart.h"

#include "gui_zfs_zpool.h"

#include "gui_zfs_arc.h"

/* ZFS 命中率明细：三列布局 */
GtkWidget *gui_zhit_detail_box(void);
void       gui_zhit_detail_update(void);

/* 近 60 秒滚动命中率曲线（界面环形缓冲） */
GtkWidget *gui_zhit_roll_chart(void);

/* ZFS 带宽明细：三列 + 延迟直方图 */
GtkWidget *gui_zbw_detail_box(void);
void       gui_zbw_detail_update(void);

#endif /* FMON_GUI_PAGE_ZFS_H */
