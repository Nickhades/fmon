/*
 * gui_runtime_internal.h - 运行时内部共享状态（快照模块 ↔ 顶部信息模块）
 */

#ifndef FMON_GUI_RUNTIME_INTERNAL_H
#define FMON_GUI_RUNTIME_INTERNAL_H

#include <gtk/gtk.h>

#include "fmon_shm.h"

#include <stdint.h>

extern FmonShmData  g_snap;
extern FmonShmData *g_live;
extern gboolean      g_online;
extern gboolean      g_ever_linked;
extern gchar        *g_link_error;
extern guint         g_ui_timer;

/* 单调时钟（微秒），由快照模块提供 */
uint64_t gui_monotonic_us(void);

#endif /* FMON_GUI_RUNTIME_INTERNAL_H */
