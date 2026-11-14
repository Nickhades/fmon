/*
 * kmem_stats.c - vmstat -z 异步采样与归类
 */

#include "kmem_stats.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static KmemSummary g_sum;
static gboolean    g_valid = FALSE;
static gint64      g_last_ms;
static gboolean    g_running = FALSE;

static gboolean zone_is(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

gboolean kmem_stats_parse(const char *text, KmemSummary *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    KmemSummary s;
    memset(&s, 0, sizeof(s));

    char **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];
        const char *colon = strrchr(line, ':');

        if (colon == NULL)
            continue;   /* 表头或空行 */

        char name[96];

        size_t nlen = (size_t)(colon - line);

        if (nlen == 0 || nlen >= sizeof(name))
            continue;

        memcpy(name, line, nlen);
        name[nlen] = '\0';

        /* 去掉名字尾部空格 */
        while (nlen > 0 && name[nlen - 1] == ' ')
            name[--nlen] = '\0';

        unsigned long long size = 0, limit = 0, used = 0, free_n = 0;
        unsigned long long req = 0, fail = 0, sleep = 0, xdom = 0;

        if (sscanf(colon + 1, " %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu",
                   &size, &limit, &used, &free_n, &req, &fail, &sleep,
                   &xdom) < 3)
            continue;

        /* "vm pgcache" 是页面缓存（可回收的缓存页），不是内核内存；
         * 它的 used 会把整机缓存都算进来，必须排除，否则总计会接近
         * 物理内存总量。 */
        if (zone_is(name, "vm pgcache"))
            continue;

        guint64 bytes = (guint64)size * (guint64)used;

        s.total_bytes += bytes;
        s.zone_count++;

        if (zone_is(name, "zio"))
            s.zio_bytes += bytes;
        else if (zone_is(name, "zfs") || zone_is(name, "dnode") ||
                 zone_is(name, "dmu_") || zone_is(name, "abd_") ||
                 zone_is(name, "arc_") || zone_is(name, "dbuf_"))
            s.zfs_bytes += bytes;
        else if (zone_is(name, "vm_") || zone_is(name, "pv_") ||
                 strstr(name, "page_table") != NULL)
            s.page_bytes += bytes;
    }

    g_strfreev(lines);

    *out = s;
    return s.zone_count > 0;
}

static void on_output(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);

    if (err != NULL)
        g_error_free(err);

    KmemSummary s;

    if (out != NULL && kmem_stats_parse(out, &s)) {
        g_sum   = s;
        g_valid = TRUE;
    }

    g_free(out);
    g_object_unref(src);
    g_running = FALSE;
}

void kmem_stats_update(void)
{
    gint64 now = g_get_monotonic_time() / 1000;

    if (g_running || (g_last_ms != 0 && now - g_last_ms < 5000))
        return;

    g_last_ms = now;

    GError     *err = NULL;
    GSubprocess *p  = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                       "vmstat", "-z", NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        return;
    }

    g_running = TRUE;
    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_output, NULL);
}

const KmemSummary *kmem_stats_get(void)
{
    return g_valid ? &g_sum : NULL;
}
