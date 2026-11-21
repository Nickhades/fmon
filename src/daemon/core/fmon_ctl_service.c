/*
 * fmon_ctl_service.c - SERVICE / SYSRC 控制命令
 */

#include "fmon_ctl_internal.h"
#include "fmon_daemon.h"

#include "fmon_daemon.h"
#include "fmon_child.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

bool ctl_service_exists(const char *name)
{
    static const char *const dirs[] = { "/etc/rc.d",
                                        FMON_LOCALBASE "/etc/rc.d" };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dirs[i], name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            return true;
    }
    return false;
}

void ctl_cmd_service(int fd, char *args)
{
    char *name = strsep(&args, " ");
    char *act  = args;

    if (name == NULL || act == NULL || !ctl_valid_token(name)) {
        ctl_reply(fd, false, "用法: SERVICE <name> <start|stop|restart>");
        return;
    }
    if (strcmp(act, "start") != 0 && strcmp(act, "onestart") != 0 &&
        strcmp(act, "stop") != 0 && strcmp(act, "restart") != 0 &&
        strcmp(act, "reload") != 0) {
        ctl_reply(fd, false, "不允许的动作: %s", act);
        return;
    }
    if (!ctl_service_exists(name)) {
        ctl_reply(fd, false, "找不到服务脚本: %s", name);
        return;
    }

    const char *argv[] = { "/usr/sbin/service", name, act, NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 64 * 1024, 15000, &res) != 0) {
        fmon_child_result_free(&res);
        ctl_reply(fd, false, "执行 service 失败");
        return;
    }

    bool ok = fmon_child_ok(&res);

    /* 只回传第一行，避免把整个服务输出塞进协议 */
    char msg[512] = "";
    if (res.out != NULL) {
        char *nl = strchr(res.out, '\n');
        if (nl != NULL)
            *nl = '\0';
        strlcpy(msg, res.out, sizeof(msg));
    }
    fmon_child_result_free(&res);

    if (ok)
        ctl_reply(fd, true, "service %s %s", name, act);
    else
        ctl_reply(fd, false, "service %s %s 失败: %s", name, act,
              (msg[0] != '\0') ? msg : "未知原因");
}

void ctl_cmd_sysrc(int fd, char *args)
{
    char *name = strsep(&args, " ");
    char *val  = args;

    if (name == NULL || val == NULL || !ctl_valid_token(name)) {
        ctl_reply(fd, false, "用法: SYSRC <name> <YES|NO>");
        return;
    }
    if (strcmp(val, "YES") != 0 && strcmp(val, "NO") != 0) {
        ctl_reply(fd, false, "只允许 YES / NO");
        return;
    }
    if (!ctl_service_exists(name)) {
        ctl_reply(fd, false, "找不到服务脚本: %s", name);
        return;
    }

    char assign[128];
    snprintf(assign, sizeof(assign), "%s_enable=%s", name, val);

    const char *argv[] = { "/usr/sbin/sysrc", assign, NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 32 * 1024, 10000, &res) != 0) {
        fmon_child_result_free(&res);
        ctl_reply(fd, false, "执行 sysrc 失败");
        return;
    }

    bool ok = fmon_child_ok(&res);
    fmon_child_result_free(&res);

    if (ok)
        ctl_reply(fd, true, "sysrc %s", assign);
    else
        ctl_reply(fd, false, "sysrc %s 失败", assign);
}
