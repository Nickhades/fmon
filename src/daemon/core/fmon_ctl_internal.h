/* fmon_ctl_internal.h - 控制命令内部接口 */
#ifndef FMON_CTL_INTERNAL_H
#define FMON_CTL_INTERNAL_H

#include <stdbool.h>

bool ctl_valid_token(const char *s);
bool ctl_service_exists(const char *name);
void ctl_reply(int fd, bool ok, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void ctl_cmd_service(int fd, char *args);
void ctl_cmd_sysrc(int fd, char *args);
void ctl_cmd_signal(int fd, char *args);
void ctl_cmd_renice(int fd, char *args);
void ctl_cmd_cpuset(int fd, char *args);

#endif
