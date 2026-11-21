/*
 * fmon_ctl_proc.c - SIGNAL / RENICE / CPUSET 控制命令
 */

#include "fmon_ctl_internal.h"

#include "fmon_child.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* CPU 列表：0,1,2 或 0-3，只允许数字、逗号、连字符 */
static bool valid_cpu_list(const char *s)
{
    if (s == NULL || s[0] == '\0' || strlen(s) > 64)
        return false;

    for (const char *p = s; *p != '\0'; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == ',' || *p == '-'))
            return false;
    }
    return true;
}

static int parse_signal(const char *name)
{
    if (strcmp(name, "TERM") == 0) return SIGTERM;
    if (strcmp(name, "KILL") == 0) return SIGKILL;
    if (strcmp(name, "STOP") == 0) return SIGSTOP;
    if (strcmp(name, "CONT") == 0) return SIGCONT;
    if (strcmp(name, "HUP") == 0)  return SIGHUP;
    return 0;
}

static bool parse_pid(const char *s, pid_t *out)
{
    char *end = NULL;
    long  v   = strtol(s, &end, 10);

    if (end == NULL || *end != '\0' || v <= 1 || v > 100000000L)
        return false;
    if ((pid_t)v == getpid())
        return false;

    *out = (pid_t)v;
    return true;
}

void ctl_cmd_signal(int fd, char *args)
{
    char *pid_s = strsep(&args, " ");
    char *sig_s = args;

    if (pid_s == NULL || sig_s == NULL) {
        ctl_reply(fd, false, "用法: SIGNAL <pid> <TERM|KILL|STOP|CONT|HUP>");
        return;
    }

    pid_t pid = 0;
    if (!parse_pid(pid_s, &pid)) {
        ctl_reply(fd, false, "非法 pid: %s", pid_s);
        return;
    }

    int sig = parse_signal(sig_s);
    if (sig == 0) {
        ctl_reply(fd, false, "不允许的信号: %s", sig_s);
        return;
    }

    if (kill(pid, sig) != 0) {
        ctl_reply(fd, false, "kill(%d) 失败: %s", (int)pid, strerror(errno));
        return;
    }

    ctl_reply(fd, true, "signal %s -> %d", sig_s, (int)pid);
}

void ctl_cmd_renice(int fd, char *args)
{
    char *pid_s  = strsep(&args, " ");
    char *nice_s = args;

    if (pid_s == NULL || nice_s == NULL) {
        ctl_reply(fd, false, "用法: RENICE <pid> <-20..19>");
        return;
    }

    pid_t pid = 0;
    if (!parse_pid(pid_s, &pid)) {
        ctl_reply(fd, false, "非法 pid: %s", pid_s);
        return;
    }

    char *end  = NULL;
    long  nice = strtol(nice_s, &end, 10);
    if (end == NULL || *end != '\0' || nice < -20 || nice > 19) {
        ctl_reply(fd, false, "非法 nice 值: %s", nice_s);
        return;
    }

    if (setpriority(PRIO_PROCESS, (id_t)pid, (int)nice) != 0) {
        ctl_reply(fd, false, "setpriority(%d) 失败: %s", (int)pid,
              strerror(errno));
        return;
    }

    ctl_reply(fd, true, "nice %ld -> %d", nice, (int)pid);
}

void ctl_cmd_cpuset(int fd, char *args)
{
    char *pid_s = strsep(&args, " ");
    char *list  = args;

    if (pid_s == NULL || list == NULL || !valid_cpu_list(list)) {
        ctl_reply(fd, false, "用法: CPUSET <pid> <0,1,2|0-3>");
        return;
    }

    pid_t pid = 0;
    if (!parse_pid(pid_s, &pid)) {
        ctl_reply(fd, false, "非法 pid: %s", pid_s);
        return;
    }

    const char *argv[] = { "/bin/cpuset", "-l", list, "-p", pid_s, NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 16 * 1024, 5000, &res) != 0) {
        fmon_child_result_free(&res);
        ctl_reply(fd, false, "执行 cpuset 失败");
        return;
    }

    bool ok = fmon_child_ok(&res);
    char msg[256] = "";

    if (res.out != NULL) {
        char *nl = strchr(res.out, '\n');
        if (nl != NULL)
            *nl = '\0';
        strlcpy(msg, res.out, sizeof(msg));
    }
    fmon_child_result_free(&res);

    if (ok)
        ctl_reply(fd, true, "cpuset %s -> %s", list, pid_s);
    else
        ctl_reply(fd, false, "cpuset 失败: %s", (msg[0] != '\0') ? msg : "未知原因");
}
