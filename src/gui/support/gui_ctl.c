/*
 * gui_ctl.c - 控制通道客户端实现
 */

#include "gui_ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_CONNECT_TIMEOUT_MS 400
#define CTL_IO_TIMEOUT_MS      4000
#define CTL_REPLY_MAX          1024

static const char *const ctl_paths[] = {
    "/var/run/fmon-ctl.sock",
    "/tmp/fmon-ctl.sock"
};

static int ctl_connect(void)
{
    for (size_t i = 0; i < sizeof(ctl_paths) / sizeof(ctl_paths[0]); i++) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strlcpy(addr.sun_path, ctl_paths[i], sizeof(addr.sun_path));

        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;

        /* 非阻塞 connect + poll，避免守护进程卡死时界面跟着卡住 */
        int flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));

        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, CTL_CONNECT_TIMEOUT_MS) > 0) {
                int       so_error = 0;
                socklen_t len      = sizeof(so_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 &&
                    so_error == 0)
                    rc = 0;
            }
        }

        if (rc != 0) {
            close(fd);
            continue;
        }

        (void)fcntl(fd, F_SETFL, flags);

        struct timeval tv = { .tv_sec = CTL_IO_TIMEOUT_MS / 1000,
                              .tv_usec = 0 };
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        return fd;
    }

    return -1;
}

static FmonCtlResult ctl_request(gchar **err, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static FmonCtlResult ctl_request(gchar **err, const char *fmt, ...)
{
    if (err != NULL)
        *err = NULL;

    int fd = ctl_connect();
    if (fd < 0) {
        if (err != NULL)
            *err = g_strdup("控制通道不可用（fmon-daemon 未运行？）");
        return FMON_CTL_UNAVAILABLE;
    }

    char    req[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(req, sizeof(req), fmt, ap);
    va_end(ap);

    if (n <= 0 || (size_t)n >= sizeof(req)) {
        close(fd);
        if (err != NULL)
            *err = g_strdup("请求过长");
        return FMON_CTL_ERROR;
    }

    size_t off = 0;
    while (off < (size_t)n) {
        ssize_t w = write(fd, req + off, (size_t)n - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }

    char    buf[CTL_REPLY_MAX];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (r <= 0) {
        if (err != NULL)
            *err = g_strdup("守护进程没有响应");
        return FMON_CTL_ERROR;
    }

    buf[r] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl != NULL)
        *nl = '\0';

    if (strncmp(buf, "OK", 2) == 0)
        return FMON_CTL_OK;

    if (err != NULL)
        *err = g_strdup((strncmp(buf, "ERR ", 4) == 0) ? buf + 4 : buf);

    return FMON_CTL_ERROR;
}

gboolean fmon_ctl_available(void)
{
    static gint64 last_check = 0;
    static gboolean cached  = FALSE;

    gint64 now = g_get_monotonic_time() / 1000;

    if (last_check != 0 && now - last_check < 10000)
        return cached;

    last_check = now;
    cached     = (ctl_request(NULL, "FMONCTL 1 PING\n") == FMON_CTL_OK);

    return cached;
}

FmonCtlResult fmon_ctl_signal(int pid, const char *sig_name, gchar **err)
{
    return ctl_request(err, "FMONCTL 1 SIGNAL %d %s\n", pid, sig_name);
}

FmonCtlResult fmon_ctl_renice(int pid, int nice, gchar **err)
{
    return ctl_request(err, "FMONCTL 1 RENICE %d %d\n", pid, nice);
}

FmonCtlResult fmon_ctl_service(const char *name, const char *action,
                               gchar **err)
{
    return ctl_request(err, "FMONCTL 1 SERVICE %s %s\n", name, action);
}

FmonCtlResult fmon_ctl_sysrc(const char *name, gboolean enable, gchar **err)
{
    return ctl_request(err, "FMONCTL 1 SYSRC %s %s\n", name,
                       enable ? "YES" : "NO");
}

FmonCtlResult fmon_ctl_cpuset(int pid, const char *cpu_list, gchar **err)
{
    return ctl_request(err, "FMONCTL 1 CPUSET %d %s\n", pid, cpu_list);
}

FmonCtlResult fmon_ctl_rctl(gboolean add, const char *rule, gchar **err)
{
    return ctl_request(err, "FMONCTL 1 RCTL %s %s\n",
                       add ? "ADD" : "REMOVE", rule);
}

FmonCtlResult fmon_ctl_rctl_list(gchar **out, gchar **err)
{
    if (out != NULL)
        *out = NULL;
    if (err != NULL)
        *err = NULL;

    int fd = ctl_connect();

    if (fd < 0) {
        if (err != NULL)
            *err = g_strdup("控制通道不可用");
        return FMON_CTL_UNAVAILABLE;
    }

    const char *req = "FMONCTL 1 RCTL LIST\n";

    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        if (err != NULL)
            *err = g_strdup("发送请求失败");
        return FMON_CTL_UNAVAILABLE;
    }

    /* 多行回复：服务端写完 "OK\n" + 规则列表后关闭连接 */
    GString *buf = g_string_new(NULL);
    char     tmp[4096];
    ssize_t  n;

    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        g_string_append_len(buf, tmp, n);

    close(fd);

    char *nl = strchr(buf->str, '\n');

    if (nl == NULL) {
        g_string_free(buf, TRUE);
        if (err != NULL)
            *err = g_strdup("守护进程没有响应");
        return FMON_CTL_ERROR;
    }

    *nl = '\0';

    if (strncmp(buf->str, "OK", 2) == 0) {
        if (out != NULL)
            *out = g_strdup(nl + 1);
        g_string_free(buf, TRUE);
        return FMON_CTL_OK;
    }

    if (err != NULL)
        *err = g_strdup((strncmp(buf->str, "ERR ", 4) == 0) ? buf->str + 4
                                                            : buf->str);

    g_string_free(buf, TRUE);
    return FMON_CTL_ERROR;
}
