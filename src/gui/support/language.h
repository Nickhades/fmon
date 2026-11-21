/*
 * language.h - 界面语言（中文 / English）
 *
 * 语言在进程启动、setlocale() 之前确定；切换后重启 GUI，
 * 避免已经创建的控件保留旧语言而出现中英混排。
 */

#ifndef FMON_LANGUAGE_H
#define FMON_LANGUAGE_H

#include <glib.h>

typedef enum {
    FMON_LANGUAGE_ZH = 0,
    FMON_LANGUAGE_EN = 1
} FmonLanguage;

/* 读取配置/系统语言，并设置 gettext 的 LANGUAGE。
 * 必须在 setlocale(LC_ALL, "") 之前调用。 */
void fmon_language_init(void);

FmonLanguage fmon_language_get(void);
const char *fmon_language_code(void);
const char *fmon_language_label(void);

/* 保存选择；实际切语言由调用方重启进程完成。 */
void fmon_language_set(FmonLanguage lang);

#endif
