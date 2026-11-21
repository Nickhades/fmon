/*
 * log_sources.h - 日志源发现（系统日志 / 包日志 / Jail 日志）
 */

#ifndef FMON_LOG_SOURCES_H
#define FMON_LOG_SOURCES_H

#include <glib.h>

typedef struct {
    gchar *label;
    gchar *path;   /* 文件源；与 cmd 二选一 */
    gchar *cmd;    /* 命令源（dmesg / zpool events） */
} LogSource;

/* 返回 LogSource* 数组，元素由 g_free 语义的 source_free 释放。 */
GPtrArray *log_sources_build(void);

#endif
