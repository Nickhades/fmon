/*
 * gui_util.h - 界面层公共工具
 *
 * 关键目标：
 *   1. 所有外部命令都在后台线程执行（GTask），绝不在 GTK 主线程里
 *      popen()/g_spawn_sync()，否则界面会卡住（旧版本的日志、进程、
 *      服务页面都有这个问题）。
 *   2. 所有子进程都通过 argv 传递参数，不经过 shell，
 *      彻底消除服务名 / 用户名 / 命令串带来的命令注入。
 */

#ifndef GUI_UTIL_H
#define GUI_UTIL_H

#include <gtk/gtk.h>

#include "fmon_child.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 命令执行完成回调。ok 表示成功执行且退出码为 0。 */
typedef void (*FmonCmdCallback)(gpointer user_data, const FmonChildResult *res,
                                gboolean ok);

/* 在后台线程执行命令；cb 在 GTK 主线程被调用。
 * user_data 的生命周期由调用者负责（回调返回后不再使用）。 */
void fmon_gui_run_async(const char *const argv[], FmonCmdCallback cb,
                        gpointer user_data);

void fmon_gui_run_async_full(const char *const argv[], size_t max_bytes,
                             int timeout_ms, FmonCmdCallback cb,
                             gpointer user_data);

/* 合并 stderr 执行（把子进程错误信息展示给用户） */
void fmon_gui_run_async_merged(const char *const argv[], FmonCmdCallback cb,
                               gpointer user_data);

/* 图表面板背景：圆角 + 上浅下深的深色渐变 + 可选的系列色描边。
 * 用于让曲线区域和窗口背景有明显区分（现代化配色）。 */
void fmon_gui_draw_panel(cairo_t *cr, double x, double y, double w, double h,
                         double border_r, double border_g, double border_b,
                         double border_a);

/* 确认对话框：用户点击 accept 按钮时调用 on_accept。 */
typedef void (*FmonConfirmCallback)(gpointer user_data);

void fmon_gui_confirm(GtkWindow *parent, const char *message, const char *detail,
                      const char *accept_label, FmonConfirmCallback on_accept,
                      gpointer user_data, GDestroyNotify user_data_free);

/* 提示框（不阻塞） */
void fmon_gui_show_info(GtkWindow *parent, const char *message,
                        const char *detail);
void fmon_gui_show_error(GtkWindow *parent, const char *message,
                         const char *detail);

/* 取当前窗口（供没有 parent 参数的回调使用），可能返回 NULL */
GtkWindow *fmon_gui_main_window(void);

/* pkexec 的绝对路径（找不到时返回 "pkexec" 交给 PATH） */

/* 简单格式化：返回新分配的字符串 */
char *fmon_gui_format_bytes(uint64_t bytes);
char *fmon_gui_format_rate(double kbps);       /* KB/s → 人类可读 */
char *fmon_gui_format_uptime(uint64_t seconds);

/* 校验服务名 / 用户名等 token 是否安全：
 * 只允许 [A-Za-z0-9._+-]，且必须以字母或数字开头（防止 -rf 之类的参数注入）。 */
gboolean fmon_gui_is_valid_token(const gchar *s);

/* 把文本安全地转成 XML（用于 label markup） */
gchar *fmon_gui_markup_escape(const gchar *text);

/* 把 ISO-8859 / 非法 UTF-8 文本清洗为合法 UTF-8（FreeBSD 日志常见） */
gchar *fmon_gui_make_valid_utf8(const gchar *data, gssize len);

#ifdef __cplusplus
}
#endif

#endif /* GUI_UTIL_H */
