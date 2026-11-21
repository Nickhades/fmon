/*
 * gui_page_prune.h - ARC Prune 明细与幽灵缓存趋势
 */

#ifndef FMON_GUI_PAGE_PRUNE_H
#define FMON_GUI_PAGE_PRUNE_H

#include <gtk/gtk.h>

GtkWidget *gui_prune_detail_box(void);
void       gui_prune_detail_update(void);
GtkWidget *gui_prune_ghost_chart(void);

#endif
