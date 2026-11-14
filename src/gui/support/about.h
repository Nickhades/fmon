/*
 * about.h - 「关于 / 许可证」对话框
 */

#ifndef ABOUT_H
#define ABOUT_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 打开关于窗口（父窗口用于模态居中） */
void fmon_about_show(GtkWindow *parent);

#ifdef __cplusplus
}
#endif

#endif /* ABOUT_H */
