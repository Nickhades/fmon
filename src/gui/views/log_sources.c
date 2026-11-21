/*
 * log_sources.c - 日志源发现
 */

#include "log_sources.h"

#include <glib.h>

#include "i18n_ui.h"

static LogSource *source_new(const char *label, const char *path,
                             const char *cmd)
{
    LogSource *s = g_new0(LogSource, 1);

    s->label = g_strdup(fmon_i18n_tr(label));
    s->path = (path != NULL) ? g_strdup(path) : NULL;
    s->cmd = (cmd != NULL) ? g_strdup(cmd) : NULL;
    return s;
}

static void source_free(gpointer data)
{
    LogSource *s = data;

    if (s == NULL)
        return;

    g_free(s->label);
    g_free(s->path);
    g_free(s->cmd);
    g_free(s);
}

static void add_source(GPtrArray *sources, const char *label,
                       const char *path, const char *cmd)
{
    g_ptr_array_add(sources, source_new(label, path, cmd));
}

static void add_file_if_exists(GPtrArray *sources, const char *label,
                               const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        add_source(sources, label, path, NULL);
}

static void scan_pkg_logs(GPtrArray *sources)
{
    GDir *d = g_dir_open("/usr/local/var/log", 0, NULL);

    if (d == NULL)
        return;

    const char *name;

    while ((name = g_dir_read_name(d)) != NULL) {
        gchar *path = g_build_filename("/usr/local/var/log", name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            gchar *label = g_strdup_printf("pkg: %s", name);

            add_source(sources, label, path, NULL);
            g_free(label);
        }

        g_free(path);
    }

    g_dir_close(d);
}

static void scan_jail_logs(GPtrArray *sources)
{
    static const char *const roots[] = { "/usr/jails", "/usr/local/jails" };

    for (size_t r = 0; r < G_N_ELEMENTS(roots); r++) {
        GDir *d = g_dir_open(roots[r], 0, NULL);

        if (d == NULL)
            continue;

        const char *jail;

        while ((jail = g_dir_read_name(d)) != NULL) {
            gchar *logdir = g_build_filename(roots[r], jail, "var", "log",
                                             NULL);
            GDir *ld = g_dir_open(logdir, 0, NULL);

            if (ld != NULL) {
                const char *lf;

                while ((lf = g_dir_read_name(ld)) != NULL) {
                    gchar *path = g_build_filename(logdir, lf, NULL);

                    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                        gchar *label = g_strdup_printf("Jail %s / %s",
                                                       jail, lf);

                        add_source(sources, label, path, NULL);
                        g_free(label);
                    }

                    g_free(path);
                }

                g_dir_close(ld);
            }

            g_free(logdir);
        }

        g_dir_close(d);
    }
}

GPtrArray *log_sources_build(void)
{
    GPtrArray *sources = g_ptr_array_new_with_free_func(source_free);

    add_source(sources, "系统消息 /var/log/messages", "/var/log/messages",
               NULL);
    add_file_if_exists(sources, "认证 /var/log/auth.log", "/var/log/auth.log");
    add_file_if_exists(sources, "安全 /var/log/security", "/var/log/security");
    add_file_if_exists(sources, "全部 /var/log/all.log", "/var/log/all.log");
    add_file_if_exists(sources, "定时任务 /var/log/cron", "/var/log/cron");
    add_file_if_exists(sources, "调试 /var/log/debug.log", "/var/log/debug.log");

    add_source(sources, "内核 dmesg（kern.msgbuf）", NULL,
               "dmesg 2>/dev/null | tail -n 400");

    if (g_find_program_in_path("zpool") != NULL)
        add_source(sources, "ZFS 事件（zpool events -v）", NULL,
                   "zpool events -v 2>/dev/null | tail -n 400");

    scan_pkg_logs(sources);
    scan_jail_logs(sources);
    return sources;
}
