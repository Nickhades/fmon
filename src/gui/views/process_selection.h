/*
 * process_selection.h - 选择保持、滚动恢复与操作可用性
 */

#ifndef FMON_PROCESS_SELECTION_H
#define FMON_PROCESS_SELECTION_H

#include "process_internal.h"

guint    proc_selection_first_pos(ProcPage *page);
void     proc_selection_select(ProcPage *page, guint pos);
guint32  proc_selection_selected_pid(ProcPage *page);
ProcItem *proc_selection_get_item(ProcPage *page);
void     proc_selection_schedule_restore(ProcPage *page, guint32 pid,
                                         double vpos);
void     proc_selection_changed(GtkSelectionModel *sel, guint position,
                                guint n_items, gpointer data);

#endif
