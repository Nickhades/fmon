/*
 * fmon_ctl.c - 守护进程控制通道实现
 *
 * 协议（一行一条，纯文本，便于用 printf/nc 调试）：
 *
 *   请求： FMONCTL 1 <CMD> [参数...]\n
 *   成功： OK <说明>\n
 *   失败： ERR <原因>\n
 *
 * 支持的 CMD：
 *   PING                          探活
 *   INFO                          版本 / ABI
 *   SIGNAL <pid> <TERM|KILL|STOP|CONT|HUP>
 *   SERVICE <rc.d 脚本名> <start|stop|restart>
 *   SYSRC <rc.d 脚本名> <YES|NO>
 *
 * 安全约定：
 *   - 只接受本地 Unix 域套接字，套接字文件 0660 且属组 operator；
 *   - 服务名必须在 /etc/rc.d 或 /usr/local/etc/rc.d 中存在（天然白名单），
 *     且只允许 [A-Za-z0-9._+-]；
 *   - 信号只允许上面 5 个；
 *   - 拒绝 pid <= 1、本进程自身；
 *   - 所有子进程都走 fmon_child（argv 直传、带超时），不经过 shell。
 */

#include "fmon_ctl.h"

#include "fmon_daemon.h"
#include "fmon_sysctl.h"
#include "fmon_child.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_PATH_RUN    "/var/run/fmon-ctl.sock"
#define CTL_PATH_TMP    "/tmp/fmon-ctl.sock"
#define CTL_MAX_REQUEST 512
#define CTL_TIMEOUT_MS  4000

static int      g_listen_fd = -1;
static char     g_sock_path[108];
static pthread_t g_thread;
static bool      g_started = false;
static volatile sig_atomic_t g_ctl_running = 0;
static gid_t g_sock_gid = (gid_t)-1;   /* 套接字属组：operator / wheel */

/* ===========================================================================
 * 校验工具
 * =========================================================================== */

/* 服务名/标识符：字母数字开头，只含 [A-Za-z0-9._+-] */
static bool valid_token(const char *s)
{
    if (s == NULL || s[0] == '\0' || strlen(s) > 64)
        return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') ||
          (s[0] >= '0' && s[0] <= '9')))
        return false;

    for (const char *p = s; *p != '\0'; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
                  *p == '+' || *p == '-';
        if (!ok)
            return false;
    }
    return true;
}

static bool service_exists(const char *name)
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

/* rctl 规则：subject:type:action[=value]，只允许安全字符 */
static bool valid_rctl_rule(const char *s)
{
    if (s == NULL || s[0] == '\0' || strlen(s) > 256)
        return false;

    for (const char *p = s; *p != '\0'; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' ||
                  *p == ',' || *p == ':' || *p == '/' || *p == '=' ||
                  *p == '+' || *p == '-';
        if (!ok)
            return false;
    }

    static const char *const types[] = {
        ":memoryuse:", ":pcpu:", ":maxproc:", ":openfiles:", ":cputime:",
        ":datasize:", ":stacksize:", ":vmemoryuse:", ":nproc:", ":nthr:",
        ":msgqbytes:", ":msgqqueued:", ":shmbytes:", ":shmseg:", ":pctrie:"
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strstr(s, types[i]) != NULL)
            return true;
    }
    return false;
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

/* ===========================================================================
 * 命令实现
 * =========================================================================== */
static void reply(int fd, bool ok, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void reply(int fd, bool ok, const char *fmt, ...)
{
    char    body[1024];
    char    line[1200];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    int n = snprintf(line, sizeof(line), "%s %s\n", ok ? "OK" : "ERR", body);
    if (n > 0)
        (void)write(fd, line, (size_t)n);
}

static void cmd_signal(int fd, char *args)
{
    char *pid_s = strsep(&args, " ");
    char *sig_s = args;

    if (pid_s == NULL || sig_s == NULL) {
        reply(fd, false, "用法: SIGNAL <pid> <TERM|KILL|STOP|CONT|HUP>");
        return;
    }

    pid_t pid = 0;
    if (!parse_pid(pid_s, &pid)) {
        reply(fd, false, "非法 pid: %s", pid_s);
        return;
    }

    int sig = parse_signal(sig_s);
    if (sig == 0) {
        reply(fd, false, "不允许的信号: %s", sig_s);
        return;
    }

    if (kill(pid, sig) != 0) {
        reply(fd, false, "kill(%d) 失败: %s", (int)pid, strerror(errno));
        return;
    }

    reply(fd, true, "signal %s -> %d", sig_s, (int)pid);
}

static void cmd_renice(int fd, char *args)
{
    char *pid_s  = strsep(&args, " ");
    char *nice_s = args;

    if (pid_s == NULL || nice_s == NULL) {
        reply(fd, false, "用法: RENICE <pid> <-20..19>");
        return;
    }

    pid_t pid = 0;
    if (!parse_pid(pid_s, &pid)) {
        reply(fd, false, "非法 pid: %s", pid_s);
        return;
    }

    char *end  = NULL;
    long  nice = strtol(nice_s, &end, 10);
    if (end == NULL || *end != '\0' || nice < -20 || nice > 19) {
        reply(fd, false, "非法 nice 值: %s", nice_s);
        return;
    }

    if (setpriority(PRIO_PROCESS, (id_t)pid, (int)nice) != 0) {
        reply(fd, false, "setpriority(%d) 失败: %s", (int)pid,
              strerror(errno));
        return;
    }

    reply(fd, true, "nice %ld -> %d", nice, (int)pid);
}

static void cmd_cpuset(int fd, char *args)
{
    char *pid_s = strsep(&args, " ");
    char *list  = args;

    if (pid_s == NULL || list == NULL || !valid_cpu_list(list)) {
        reply(fd, false, "用法: CPUSET <pid> <0,1,2|0-3>");
        return;
    }

    pid_t pid = 0;
    if (!parse_pid(pid_s, &pid)) {
        reply(fd, false, "非法 pid: %s", pid_s);
        return;
    }

    const char *argv[] = { "/bin/cpuset", "-l", list, "-p", pid_s, NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 16 * 1024, 5000, &res) != 0) {
        fmon_child_result_free(&res);
        reply(fd, false, "执行 cpuset 失败");
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
        reply(fd, true, "cpuset %s -> %s", list, pid_s);
    else
        reply(fd, false, "cpuset 失败: %s", (msg[0] != '\0') ? msg : "未知原因");
}

/* 列出规则：先回 OK 行，再原样输出（多行），由客户端读到 EOF */
static void cmd_rctl_list(int fd)
{
    const char *argv[] = { "/usr/bin/rctl", "-h", NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 256 * 1024, 5000, &res) != 0) {
        fmon_child_result_free(&res);
        reply(fd, false, "执行 rctl 失败");
        return;
    }

    if (res.out != NULL && strstr(res.out, "disabled") != NULL) {
        reply(fd, false, "RACCT/RCTL 未启用：%s", res.out);
        fmon_child_result_free(&res);
        return;
    }

    if (!fmon_child_ok(&res)) {
        reply(fd, false, "rctl 失败：%s",
              (res.out != NULL) ? res.out : "未知原因");
        fmon_child_result_free(&res);
        return;
    }

    (void)write(fd, "OK\n", 3);

    if (res.out != NULL && res.out_len > 0)
        (void)write(fd, res.out, res.out_len);

    fmon_child_result_free(&res);
}

static void cmd_rctl(int fd, char *args)
{
    char *verb = strsep(&args, " ");
    char *rule = args;

    if (verb != NULL && strcmp(verb, "LIST") == 0) {
        cmd_rctl_list(fd);
        return;
    }

    if (verb == NULL || rule == NULL ||
        (strcmp(verb, "ADD") != 0 && strcmp(verb, "REMOVE") != 0) ||
        !valid_rctl_rule(rule)) {
        reply(fd, false, "用法: RCTL <ADD|REMOVE> <rule> 或 RCTL LIST");
        return;
    }

    const char *argv[] = { "/usr/bin/rctl",
                           (strcmp(verb, "ADD") == 0) ? "-a" : "-r", rule,
                           NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 32 * 1024, 5000, &res) != 0) {
        fmon_child_result_free(&res);
        reply(fd, false, "执行 rctl 失败");
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
        reply(fd, true, "rctl %s %s", verb, rule);
    else
        reply(fd, false, "rctl 失败: %s", (msg[0] != '\0') ? msg : "未知原因");
}

static void cmd_service(int fd, char *args)
{
    char *name = strsep(&args, " ");
    char *act  = args;

    if (name == NULL || act == NULL || !valid_token(name)) {
        reply(fd, false, "用法: SERVICE <name> <start|stop|restart>");
        return;
    }
    if (strcmp(act, "start") != 0 && strcmp(act, "onestart") != 0 &&
        strcmp(act, "stop") != 0 && strcmp(act, "restart") != 0 &&
        strcmp(act, "reload") != 0) {
        reply(fd, false, "不允许的动作: %s", act);
        return;
    }
    if (!service_exists(name)) {
        reply(fd, false, "找不到服务脚本: %s", name);
        return;
    }

    const char *argv[] = { "/usr/sbin/service", name, act, NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 64 * 1024, 15000, &res) != 0) {
        fmon_child_result_free(&res);
        reply(fd, false, "执行 service 失败");
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
        reply(fd, true, "service %s %s", name, act);
    else
        reply(fd, false, "service %s %s 失败: %s", name, act,
              (msg[0] != '\0') ? msg : "未知原因");
}

static void cmd_sysrc(int fd, char *args)
{
    char *name = strsep(&args, " ");
    char *val  = args;

    if (name == NULL || val == NULL || !valid_token(name)) {
        reply(fd, false, "用法: SYSRC <name> <YES|NO>");
        return;
    }
    if (strcmp(val, "YES") != 0 && strcmp(val, "NO") != 0) {
        reply(fd, false, "只允许 YES / NO");
        return;
    }
    if (!service_exists(name)) {
        reply(fd, false, "找不到服务脚本: %s", name);
        return;
    }

    char assign[128];
    snprintf(assign, sizeof(assign), "%s_enable=%s", name, val);

    const char *argv[] = { "/usr/sbin/sysrc", assign, NULL };

    FmonChildResult res;
    if (fmon_child_run_merged(argv, 32 * 1024, 10000, &res) != 0) {
        fmon_child_result_free(&res);
        reply(fd, false, "执行 sysrc 失败");
        return;
    }

    bool ok = fmon_child_ok(&res);
    fmon_child_result_free(&res);

    if (ok)
        reply(fd, true, "sysrc %s", assign);
    else
        reply(fd, false, "sysrc %s 失败", assign);
}

/* ===========================================================================
 * 连接处理
 * =========================================================================== */
static void handle_connection(int fd)
{
    struct timeval tv = { .tv_sec = CTL_TIMEOUT_MS / 1000,
                          .tv_usec = 0 };

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 审计：记录是谁发起的 */
    uid_t peer_uid = (uid_t)-1;
    gid_t peer_gid = (gid_t)-1;
    if (getpeereid(fd, &peer_uid, &peer_gid) == 0) {
        /* 内核在 connect() 时已经按“补充组”检查过套接字文件权限，
         * 这里再做一次显式校验：root、守护进程自身、或套接字属组的成员
         * 才允许。注意 getpeereid 只给主组，必须用 getgrouplist() 查
         * 补充组，否则会把普通桌面用户误拒。 */
        if (peer_uid != 0 && peer_uid != getuid()) {
            bool allowed = false;

            struct passwd *pw = getpwuid(peer_uid);
            if (pw != NULL && g_sock_gid != (gid_t)-1) {
                gid_t groups[32];
                int   ngroups = (int)(sizeof(groups) / sizeof(groups[0]));

                if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) >= 0) {
                    for (int i = 0; i < ngroups; i++) {
                        if (groups[i] == g_sock_gid) {
                            allowed = true;
                            break;
                        }
                    }
                }
            }

            if (!allowed) {
                fprintf(stderr, "[FMON-CTL] 拒绝来自 uid=%d 的请求\n",
                        (int)peer_uid);
                reply(fd, false, "权限不足（需要 root 或套接字属组成员）");
                return;
            }
        }
    }

    char buf[CTL_MAX_REQUEST + 1];
    size_t len = 0;

    while (len < CTL_MAX_REQUEST) {
        ssize_t n = read(fd, buf + len, CTL_MAX_REQUEST - len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (n == 0)
            break;
        len += (size_t)n;
        if (memchr(buf, '\n', len) != NULL)
            break;
    }
    buf[len] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl != NULL)
        *nl = '\0';

    /* 协议头 */
    if (strncmp(buf, "FMONCTL 1 ", 10) != 0) {
        reply(fd, false, "协议不匹配，期望: FMONCTL 1 <CMD> ...");
        return;
    }

    char *cmd  = buf + 10;
    char *args = strchr(cmd, ' ');

    if (args != NULL) {
        *args++ = '\0';
        while (*args == ' ')
            args++;
    }

    if (strcmp(cmd, "PING") == 0) {
        reply(fd, true, "pong");
    } else if (strcmp(cmd, "INFO") == 0) {
        reply(fd, true, "fmon %s abi %u", FMON_VERSION_STRING,
              FMON_ABI_VERSION);
    } else if (strcmp(cmd, "SIGNAL") == 0) {
        if (args == NULL)
            reply(fd, false, "缺少参数");
        else
            cmd_signal(fd, args);
    } else if (strcmp(cmd, "RENICE") == 0) {
        if (args == NULL)
            reply(fd, false, "缺少参数");
        else
            cmd_renice(fd, args);
    } else if (strcmp(cmd, "CPUSET") == 0) {
        if (args == NULL)
            reply(fd, false, "缺少参数");
        else
            cmd_cpuset(fd, args);
    } else if (strcmp(cmd, "RCTL") == 0) {
        if (args == NULL)
            reply(fd, false, "缺少参数");
        else
            cmd_rctl(fd, args);
    } else if (strcmp(cmd, "SERVICE") == 0) {
        if (args == NULL)
            reply(fd, false, "缺少参数");
        else
            cmd_service(fd, args);
    } else if (strcmp(cmd, "SYSRC") == 0) {
        if (args == NULL)
            reply(fd, false, "缺少参数");
        else
            cmd_sysrc(fd, args);
    } else {
        reply(fd, false, "未知命令: %s", cmd);
    }
}

static void *ctl_thread(void *arg)
{
    (void)arg;

    while (g_ctl_running) {
        struct pollfd p = { .fd = g_listen_fd, .events = POLLIN };
        int r = poll(&p, 1, 500);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            continue;

        int c = accept(g_listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            break;
        }

        handle_connection(c);
        close(c);
    }

    return NULL;
}

/* ===========================================================================
 * 启停
 * =========================================================================== */
bool fmon_ctl_start(void)
{
    if (g_started)
        return true;

    const char *path = CTL_PATH_TMP;

    /* 优先 /var/run（root 运行时），否则退回 /tmp（便于本地调试） */
    if (access("/var/run", W_OK) == 0)
        path = CTL_PATH_RUN;

    snprintf(g_sock_path, sizeof(g_sock_path), "%s", path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path));

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "[FMON-CTL] socket 失败: %s\n", strerror(errno));
        return false;
    }

    unlink(g_sock_path);   /* 清理上次异常退出的残留 */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[FMON-CTL] bind(%s) 失败: %s\n", g_sock_path,
                strerror(errno));
        close(fd);
        return false;
    }

    if (chmod(g_sock_path, 0660) != 0) {
        fprintf(stderr, "[FMON-CTL] chmod 失败: %s\n", strerror(errno));
        close(fd);
        unlink(g_sock_path);
        return false;
    }

    /* 属组 operator（没有则 wheel），让桌面用户组成员可以连接 */
    if (geteuid() == 0) {
        struct group *gr = getgrnam("operator");

        if (gr == NULL)
            gr = getgrnam("wheel");
        if (gr != NULL) {
            (void)chown(g_sock_path, (uid_t)-1, gr->gr_gid);
            g_sock_gid = gr->gr_gid;
        }
    }

    if (g_sock_gid == (gid_t)-1)
        g_sock_gid = getgid();

    if (listen(fd, 8) != 0) {
        fprintf(stderr, "[FMON-CTL] listen 失败: %s\n", strerror(errno));
        close(fd);
        unlink(g_sock_path);
        return false;
    }

    g_listen_fd = fd;
    g_ctl_running = 1;

    if (pthread_create(&g_thread, NULL, ctl_thread, NULL) != 0) {
        fprintf(stderr, "[FMON-CTL] 创建线程失败\n");
        close(fd);
        g_listen_fd = -1;
        g_ctl_running = 0;
        unlink(g_sock_path);
        return false;
    }

    g_started = true;
    return true;
}

void fmon_ctl_stop(void)
{
    if (!g_started)
        return;

    g_ctl_running = 0;

    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    pthread_join(g_thread, NULL);

    unlink(g_sock_path);
    g_started = false;
}
