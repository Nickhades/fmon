/*
 * log_filter.h - 日志级别/时间范围/模式匹配纯函数
 */

#ifndef FMON_LOG_FILTER_H
#define FMON_LOG_FILTER_H

#include <glib.h>

typedef enum {
    LOG_FILTER_ALL = 0,
    LOG_FILTER_EMERG,
    LOG_FILTER_ALERT,
    LOG_FILTER_CRIT,
    LOG_FILTER_ERR,
    LOG_FILTER_WARN,
    LOG_FILTER_NOTICE,
    LOG_FILTER_INFO,
    LOG_FILTER_DEBUG
} LogFilter;

/* 0 emerg … 7 debug；无法识别按 info(6)。 */
int fmon_log_syslog_rank(const gchar *lower);

/* 当前级别过滤是否接受该行。 */
gboolean fmon_log_level_matches(const gchar *line, LogFilter filter);

/* 时间范围过滤；无法解析时间戳时保留。seconds <= 0 表示全部。 */
gboolean fmon_log_in_range(const gchar *line, gint64 seconds);

/* 所有 pattern 都命中才通过；支持正则/大小写敏感。 */
gboolean fmon_log_pattern_match(const gchar *line, GPtrArray *patterns,
                                gboolean regex, gboolean case_sensitive);

/* 去重键：有 "]: " 时取消息体，否则整行。 */
gchar *fmon_log_dedup_key(const gchar *line);

#endif
