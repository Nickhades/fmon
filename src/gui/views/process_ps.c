/*
 * process_ps.c - ps 输出解析与内核 CPU 占用读取
 *
 * 除 kern.proc.all 之外只依赖 GLib：解析函数都是纯函数，
 * 便于 tests/test_process_ps.c 直接测试。
 */

#include "process_ps.h"

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <libprocstat.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

/* ---------------------------------------------------------------------------
 * 进程分类：桌面应用 / 用户进程 / 系统组件
 *
 * 目的：列表里把“用户打开的桌面软件”放前面、“系统/桌面基础组件”放后面，
 * 避免误杀 kwin/plasma/dbus 这类进程。名字都按 basename 小写比较。
 * ------------------------------------------------------------------------- */
static const char *const SYSTEM_NAMES[] = {
    "kwin_x11", "kwin_wayland", "plasmashell", "plasma_session", "plasma",
    "kded5", "kded6", "ksecretd", "kaccess", "kactivitymanagerd",
    "ksmserver", "klauncher", "kwalletd5", "kwalletd6", "kglobalacceld",
    "xdg-desktop-portal", "xdg-desktop-portal-kde", "xdg-document-portal",
    "xdg-permission-store", "dbus-daemon", "dbus-broker", "dbus-launch",
    "syncthing", "pulseaudio", "pipewire", "pipewire-pulse", "wireplumber",
    "fcitx5", "sshd", "cron", "syslogd", "devd", "moused", "ntpd", "sddm",
    "networkmanager", "wpa_supplicant", "dhclient", "inetd", "rpcbind",
    "mountd", "nfsd", "automountd", "powerd", "apmd", "smartd",
    "systemd", "init", "launchd", "nginx", "postgres", "mysqld",
    "redis-server", "dockerd", "containerd", NULL,
};

static const char *const DESKTOP_NAMES[] = {
    "chrome", "chromium", "chromium-browser", "firefox", "firefox-esr",
    "thunderbird", "konsole", "dolphin", "kate", "gwenview", "okular",
    "spectacle", "ark", "code", "codium", "libreoffice", "soffice", "vlc",
    "mpv", "obs", "obs-studio", "telegram", "telegram-desktop", "qq",
    "wechat", "spotify", "steam", "discord", "fmon-gui", "kitty",
    "alacritty", "foot", "gimp", "inkscape", "blender", "krita",
    "kcalc", NULL,
};

static const char *basename_lower(const char *cmd, char *buf, size_t len)
{
    /* 先截到 argv[0]（遇到第一个空白为止），再取这一段里的最后一个 '/'。
     * 不能直接在整条命令行上 strrchr('/')：参数里也可能带路径。 */
    size_t tok = strcspn(cmd, " \t");
    size_t off = 0;

    for (size_t i = 0; i < tok; i++) {
        if (cmd[i] == '/')
            off = i + 1;
    }

    size_t n = tok - off;

    if (n >= len)
        n = len - 1;

    memcpy(buf, cmd + off, n);
    buf[n] = '\0';

    /* FreeBSD 的 ps 对部分进程输出 "name: args"（带冒号），去掉它 */
    if (n > 0 && buf[n - 1] == ':')
        buf[n - 1] = '\0';

    for (char *p = buf; *p != '\0'; p++)
        *p = g_ascii_tolower(*p);

    return buf;
}

static gboolean in_list(const char *name, const char *const *list)
{
    for (int i = 0; list[i] != NULL; i++) {
        if (strcmp(name, list[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

guint8 proc_classify_category(const gchar *command, guint32 uid,
                              gboolean is_system)
{
    if (is_system || uid == 0)
        return 2;

    if (command == NULL)
        return 1;

    char name[128];

    basename_lower(command, name, sizeof(name));

    if (in_list(name, SYSTEM_NAMES) ||
        g_str_has_prefix(command, "daemon:") ||
        g_strrstr(name, "daemon") != NULL)
        return 2;

    if (in_list(name, DESKTOP_NAMES) || strstr(command, "/chrome") != NULL ||
        strstr(command, "/chromium") != NULL ||
        strstr(command, "/firefox") != NULL)
        return 0;

    return 1;
}

const gchar *proc_category_name(guint8 category)
{
    switch (category) {
    case 0:  return "桌面";
    case 2:  return "系统";
    default: return "用户";
    }
}

/* ---------------------------------------------------------------------------
 * 内核 CPU 占用（与 top 的 WCPU 同源）
 *
 * ps 的 pcpu 只打印一位小数，空闲进程全是 0.0；kern.proc.all 里的
 * ki_pctcpu 是定点数（FSCALE=2048），换算后能得到两位以上精度。
 * FreeBSD 13+ 每个线程一条记录，所以按 pid 求和（与 top 的做法一致）。
 * ------------------------------------------------------------------------- */
GHashTable *proc_ps_parse_kproc(const void *buf, size_t len)
{
    GHashTable *map =
        g_hash_table_new_full(g_int_hash, g_int_equal, g_free, g_free);
    const struct kinfo_proc *kp = buf;
    size_t                   n  = (buf != NULL) ? len / sizeof(*kp) : 0;

    for (size_t i = 0; i < n; i++) {
        int pid = kp[i].ki_pid;

        if (pid <= 0)
            continue;

        double  add = (double)kp[i].ki_pctcpu / (double)FSCALE * 100.0;
        double *cur = g_hash_table_lookup(map, &pid);

        if (cur != NULL) {
            *cur += add;
            continue;
        }

        int    *key = g_new(int, 1);
        double *val = g_new(double, 1);

        *key = pid;
        *val = add;
        g_hash_table_replace(map, key, val);
    }

    return map;
}

GHashTable *proc_ps_read_wcpu(void)
{
    size_t len = 0;

    if (sysctlbyname("kern.proc.all", NULL, &len, NULL, 0) != 0 || len == 0)
        return NULL;

    void *buf = g_malloc(len);

    /* 两次调用之间进程数可能增长，ENOMEM 时放宽一点重试一次 */
    if (sysctlbyname("kern.proc.all", buf, &len, NULL, 0) != 0) {
        size_t bigger = len + len / 4;
        void  *nbuf   = g_try_realloc(buf, bigger);

        if (nbuf == NULL) {
            g_free(buf);
            return NULL;
        }
        buf = nbuf;
        len = bigger;

        if (sysctlbyname("kern.proc.all", buf, &len, NULL, 0) != 0) {
            g_free(buf);
            return NULL;
        }
    }

    GHashTable *map = proc_ps_parse_kproc(buf, len);

    g_free(buf);
    return map;
}

GHashTable *proc_ps_read_extras(void)
{
    size_t len = 0;

    if (sysctlbyname("kern.proc.all", NULL, &len, NULL, 0) != 0 || len == 0)
        return NULL;

    void *buf = g_malloc(len);

    if (sysctlbyname("kern.proc.all", buf, &len, NULL, 0) != 0) {
        g_free(buf);
        return NULL;
    }

    GHashTable     *map = g_hash_table_new_full(g_int_hash, g_int_equal,
                                                g_free, g_free);
    const struct kinfo_proc *kp = buf;
    size_t n = len / sizeof(*kp);
    long   page = sysconf(_SC_PAGESIZE);

    if (page <= 0)
        page = 4096;

    for (size_t i = 0; i < n; i++) {
        int pid = kp[i].ki_pid;

        if (pid <= 0)
            continue;

        ProcExtra *e = g_hash_table_lookup(map, &pid);

        if (e == NULL) {
            int *key = g_new(int, 1);

            *key = pid;
            e = g_new0(ProcExtra, 1);
            g_hash_table_insert(map, key, e);
        }

        guint64 rss = (guint64)kp[i].ki_rssize * (guint64)page / 1024ULL;
        guint64 swr = (guint64)kp[i].ki_swrss * (guint64)page / 1024ULL;
        double  ct = (double)kp[i].ki_rusage.ru_utime.tv_sec +
                     (double)kp[i].ki_rusage.ru_utime.tv_usec / 1e6 +
                     (double)kp[i].ki_rusage.ru_stime.tv_sec +
                     (double)kp[i].ki_rusage.ru_stime.tv_usec / 1e6;

        if (rss > e->rss_kb)
            e->rss_kb = rss;
        if (swr > e->swap_kb)
            e->swap_kb = swr;
        if (ct > e->cputime_s)
            e->cputime_s = ct;
        if ((guint32)kp[i].ki_numthreads > e->threads)
            e->threads = (guint32)kp[i].ki_numthreads;

        e->ppid = (guint32)kp[i].ki_ppid;
    }

    g_free(buf);
    return map;
}

/* 用 libprocstat 直接生成与 ps 相同列格式的文本，供现有解析器复用。
 * 成功时不再 fork ps。 */
gchar *proc_read_libprocstat_text(void)
{
    struct procstat *ps = procstat_open_sysctl();

    if (ps == NULL)
        return NULL;

    unsigned int n = 0;
    struct kinfo_proc *procs = procstat_getprocs(ps, KERN_PROC_PROC, 0, &n);

    if (procs == NULL) {
        procstat_close(ps);
        return NULL;
    }

    long page = sysconf(_SC_PAGESIZE);

    if (page <= 0)
        page = 4096;

    GString *out = g_string_new(NULL);

    for (unsigned int i = 0; i < n; i++) {
        struct kinfo_proc *kp = &procs[i];

        if (kp->ki_pid <= 0)
            continue;

        char state;

        switch (kp->ki_stat) {
        case SIDL:   state = 'I'; break;
        case SRUN:   state = 'R'; break;
        case SSLEEP: state = 'S'; break;
        case SSTOP:  state = 'T'; break;
        case SZOMB:  state = 'Z'; break;
        case SWAIT:  state = 'W'; break;
        case SLOCK:  state = 'D'; break;
        default:     state = '?'; break;
        }

        struct passwd *pw = getpwuid(kp->ki_uid);
        const char    *user = (pw != NULL) ? pw->pw_name : "?";
        double cpu = (double)kp->ki_pctcpu / (double)FSCALE * 100.0;
        guint64 rss = (guint64)kp->ki_rssize * (guint64)page / 1024ULL;

        char **argv = procstat_getargv(ps, kp, 0);
        GString *cmd = g_string_new(NULL);

        if (argv != NULL) {
            for (int a = 0; argv[a] != NULL; a++) {
                if (a > 0)
                    g_string_append_c(cmd, ' ');
                g_string_append(cmd, argv[a]);
            }
            procstat_freeargv(ps);
        }

        if (cmd->len == 0)
            g_string_append(cmd, kp->ki_comm);

        for (char *p = cmd->str; *p != '\0'; p++)
            if (*p == '\n' || *p == '\r')
                *p = ' ';

        g_string_append_printf(out, "%d %u %s %.2f %.1f %llu %c %s\n",
                               kp->ki_pid, kp->ki_uid, user, cpu, 0.0,
                               (unsigned long long)rss, state, cmd->str);
        g_string_free(cmd, TRUE);
    }

    procstat_freeprocs(ps, procs);
    procstat_close(ps);

    return g_string_free(out, FALSE);
}

void proc_ps_row_free(gpointer data)
{
    PsRow *row = data;

    if (row == NULL)
        return;
    g_free(row->user);
    g_free(row->comm);
    g_free(row);
}

/* 跳过本程序自己拉起的 ps 辅助进程，避免列表里出现刷新的残影 */
static gboolean is_our_ps_helper(const gchar *cmd)
{
    return (strncmp(cmd, "env LC_NUMERIC=C ps ", 20) == 0 ||
            strncmp(cmd, "ps -", 4) == 0);
}

guint64 proc_parse_ps_rows(const gchar *output, GPtrArray *rows)
{
    guint64 fp = 1469598103934665603ULL;

    if (output == NULL)
        return 0;

    GHashTable *seen =
        g_hash_table_new_full(g_int_hash, g_int_equal, g_free, NULL);
    gchar **lines = g_strsplit(output, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;

        int      pid  = 0;
        unsigned uid  = 0;
        char     user[32] = {0};
        double   cpu  = 0.0;
        double   mem  = 0.0;
        unsigned long rss = 0;
        char     stat[16] = {0};
        int      offset = 0;

        /* 表头行会在 pid 解析处失败，自然被跳过 */
        if (sscanf(lines[i], "%d %u %31s %lf %lf %lu %15s %n", &pid, &uid,
                   user, &cpu, &mem, &rss, stat, &offset) != 7)
            continue;

        if (pid <= 0)
            continue;

        /* kvm/ps 在某些版本会把线程也列出来，按 pid 去重 */
        if (g_hash_table_contains(seen, &pid))
            continue;

        const gchar *cmd = lines[i] + offset;
        while (*cmd == ' ')
            cmd++;

        if (is_our_ps_helper(cmd))
            continue;

        int *key = g_new(int, 1);
        *key = pid;
        g_hash_table_add(seen, key);

        gchar *cmd_clean = g_strchomp(g_strdup(cmd));
        gchar *cmd_valid = g_utf8_make_valid(cmd_clean, -1);
        g_free(cmd_clean);

        PsRow *row = g_new0(PsRow, 1);
        row->pid       = (guint32)pid;
        row->uid       = (guint32)uid;
        row->cpu       = cpu;
        row->mem       = mem;
        row->rss_kb    = (guint64)rss;
        row->state     = (stat[0] != '\0') ? stat[0] : '?';
        row->user      = g_utf8_make_valid(user, -1);
        row->comm      = cmd_valid;
        /* uid 0 / 系统账户 / nobody(65534) 都归为系统进程 */
        row->is_system = (uid == 0 || uid < 1000 || uid >= 65534);

        g_ptr_array_add(rows, row);

        fp ^= (guint64)pid;
        fp *= 1099511628211ULL;
        fp ^= (guint64)(cpu * 10.0);
        fp *= 1099511628211ULL;
        fp ^= (guint64)(mem * 10.0);
        fp *= 1099511628211ULL;
        fp ^= row->rss_kb;
        fp *= 1099511628211ULL;
        fp ^= (guint64)(unsigned char)row->state;
        fp *= 1099511628211ULL;
    }

    g_strfreev(lines);
    g_hash_table_destroy(seen);

    return fp;
}
