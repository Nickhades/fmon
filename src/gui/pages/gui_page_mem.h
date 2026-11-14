/*
 * gui_page_mem.h - 内存条详情
 */

#ifndef FMON_GUI_PAGE_MEM_H
#define FMON_GUI_PAGE_MEM_H

#include <gtk/gtk.h>

/* 内存条信息：放在页面标题行右侧（不再单独占一张卡片） */
GtkWidget *gui_mem_modules_header(void);
void gui_mem_modules_update(void);

/* 内存队列（活跃/非活跃/待回收/常驻/缓存/空闲）与回收速率；
 * 直接嵌在“内存”大卡片里，不单独成卡 */
GtkWidget *gui_mem_queues_box(void);

/* 内存/Swap 总容量摘要（放在标题行右侧，替代底部卡片） */
GtkWidget *gui_mem_totals_box(void);
void       gui_mem_totals_update(void);
void gui_mem_queues_update(void);

#endif /* FMON_GUI_PAGE_MEM_H */
