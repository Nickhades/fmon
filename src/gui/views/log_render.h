/*
 * log_render.h - 日志过滤/渲染
 */

#ifndef FMON_LOG_RENDER_H
#define FMON_LOG_RENDER_H

#include "log_internal.h"

GString  *log_render_filter(LogPage *page, const gchar *text);
gboolean  log_render_at_bottom(LogPage *page);
void      log_render_apply_pid_tags(GtkTextBuffer *buffer);
gchar    *log_render_raw_context(LogPage *page, const gchar *needle);
void      log_render_refilter(LogPage *page);

#endif
