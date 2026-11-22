/*
 * csv_util.h - CSV 文本字段转义
 */

#ifndef FMON_CSV_UTIL_H
#define FMON_CSV_UTIL_H

#include <glib.h>

/* 返回新分配的、带双引号的 CSV 字段；逗号/引号/换行安全，
 * 以 = + - @ 或制表符开头的文本加 apostrophe 前缀，防止表格公式注入。 */
char *fmon_csv_escape(const char *text);

#endif
