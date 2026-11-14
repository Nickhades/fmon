/*
 * fmon_ctl.h - 守护进程控制通道（Unix 域套接字）
 *
 * 目的：把“结束进程 / 启停服务”这类需要 root 的操作，从 GUI 反复调用
 * pkexec（每次弹密码框）改成守护进程内部执行。好处：
 *   1. 用户体验：不再每次输密码；
 *   2. 安全面：GUI 不再向 polkit 传任意命令，参数由守护进程严格校验；
 *   3. 可审计：所有操作都在 fmon-daemon 里集中处理并记录。
 *
 * 访问控制：
 *   - 套接字文件权限 0660，属组为 operator（不存在则 wheel）；
 *   - 连接时内核按进程的补充组做权限检查，非该组成员无法 connect；
 *   - 服务端再用 getpeereid() 记录对端 uid，便于审计。
 */

#ifndef FMON_CTL_H
#define FMON_CTL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动控制通道（失败不影响采集，只是 GUI 会回退到 pkexec） */
bool fmon_ctl_start(void);

/* 停止控制通道并删除套接字 */
void fmon_ctl_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* FMON_CTL_H */
