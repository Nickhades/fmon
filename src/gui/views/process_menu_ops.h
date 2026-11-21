/*
 * process_menu_ops.h - 进程树/批量结束菜单操作
 */

#ifndef FMON_PROCESS_MENU_OPS_H
#define FMON_PROCESS_MENU_OPS_H

#include <gtk/gtk.h>

#include "process_internal.h"

void on_menu_term_tree(GtkButton *btn, gpointer data);
void on_menu_kill_tree(GtkButton *btn, gpointer data);
void on_menu_batch_term(GtkButton *btn, gpointer data);
void on_menu_batch_kill(GtkButton *btn, gpointer data);
gboolean process_menu_selection_all_owned(ProcPage *page);

#endif
