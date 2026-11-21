/*
 * process_tree.h - 进程树排序与树形模式切换
 */

#ifndef FMON_PROCESS_TREE_H
#define FMON_PROCESS_TREE_H

#include <gtk/gtk.h>

#include "process_internal.h"

void proc_tree_sort_items(ProcItem **items, guint n, ProcPage *page);
void proc_tree_sorter_changed(GtkSorter *sorter, GParamSpec *pspec,
                              gpointer data);
void proc_tree_toggled(GtkToggleButton *btn, gpointer data);

#endif
