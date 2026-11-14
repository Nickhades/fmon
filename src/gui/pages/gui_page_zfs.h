/*
 * gui_page_zfs.h - ZFS 池页：右上摘要 + 详情（性能/健康/vdev）
 */

#ifndef FMON_GUI_PAGE_ZFS_H
#define FMON_GUI_PAGE_ZFS_H

#include <gtk/gtk.h>

#include "gui_chart.h"

/* 右上角摘要：池名/状态、已用/总量、使用率/空闲（hw-model 字号） */
GtkWidget *gui_zpool_header_box(void);

/* 详情卡片：延迟/队列/错误/碎片/TXG/vdev/scrub */
GtkWidget *gui_zpool_detail_box(void);

void gui_zpool_detail_update(void);

/* 错误趋势曲线（新增错误/秒），由 gui_zpool_detail_update 采样 */
CurveData *gui_zpool_err_curve(void);

/* ARC 明细：三列布局（大小/目标值；构成；元数据/节流） */
GtkWidget *gui_arc_detail_box(void);
void       gui_arc_detail_update(void);

/* ARC 构成趋势：数据缓冲 vs 元数据（GUI 环形缓冲） */
GtkWidget *gui_arc_trend_chart(void);

/* ZFS 命中率明细：三列布局 */
GtkWidget *gui_zhit_detail_box(void);
void       gui_zhit_detail_update(void);

/* 近 60 秒滚动命中率曲线（界面环形缓冲） */
GtkWidget *gui_zhit_roll_chart(void);

/* ZFS 带宽明细：三列 + 延迟直方图 */
GtkWidget *gui_zbw_detail_box(void);
void       gui_zbw_detail_update(void);

/* ARC Prune 回收明细：三列 + 回收风暴判据 */
GtkWidget *gui_prune_detail_box(void);
void       gui_prune_detail_update(void);

/* 幽灵缓存趋势（MRU / MFU ghost size） */
GtkWidget *gui_prune_ghost_chart(void);

#endif /* FMON_GUI_PAGE_ZFS_H */
