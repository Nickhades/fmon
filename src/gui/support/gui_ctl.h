/*
 * gui_ctl.h - 控制通道客户端
 *
 * 与 fmon-daemon 的 Unix 域套接字通信；守护进程未运行时返回
 * FMON_CTL_UNAVAILABLE，调用方回退到 pkexec。
 */

#ifndef GUI_CTL_H
#define GUI_CTL_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMON_CTL_OK = 0,     /* 命令执行成功 */
    FMON_CTL_UNAVAILABLE,/* 守护进程没运行 / 套接字不可用 → 调用方回退 pkexec */
    FMON_CTL_ERROR       /* 守护进程明确拒绝或执行失败，err 里有原因 */
} FmonCtlResult;

/* 守护进程是否在线（10 秒缓存，避免每次操作都探测） */
gboolean fmon_ctl_available(void);

/* 以下函数：成功返回 FMON_CTL_OK；err 非 NULL 时由调用者 g_free() */
FmonCtlResult fmon_ctl_signal(int pid, const char *sig_name, gchar **err);
FmonCtlResult fmon_ctl_renice(int pid, int nice, gchar **err);
FmonCtlResult fmon_ctl_service(const char *name, const char *action,
                               gchar **err);
FmonCtlResult fmon_ctl_sysrc(const char *name, gboolean enable, gchar **err);
FmonCtlResult fmon_ctl_cpuset(int pid, const char *cpu_list, gchar **err);
FmonCtlResult fmon_ctl_rctl(gboolean add, const char *rule, gchar **err);

/* 列出 rctl 规则（多行输出，out 由调用者 g_free） */
FmonCtlResult fmon_ctl_rctl_list(gchar **out, gchar **err);

#ifdef __cplusplus
}
#endif

#endif /* GUI_CTL_H */
