/*
 * long_history.h - 主界面 1h/24h 长历史模式
 */
#ifndef FMON_LONG_HISTORY_H
#define FMON_LONG_HISTORY_H

#include <glib.h>
#include <stdbool.h>

bool   gui_long_history_enter(gint64 range_secs);
void   gui_long_history_leave(void);
void   gui_long_history_refresh(void);
bool   gui_long_history_active(void);
gint64 gui_long_history_range(void);

#endif /* FMON_LONG_HISTORY_H */
