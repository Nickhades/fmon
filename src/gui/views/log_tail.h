/*
 * log_tail.h - 日志增量读取与文件监听
 */

#ifndef FMON_LOG_TAIL_H
#define FMON_LOG_TAIL_H

#include "log_internal.h"

void log_tail_start(LogPage *page);
void log_tail_setup_monitor(LogPage *page);

#endif
