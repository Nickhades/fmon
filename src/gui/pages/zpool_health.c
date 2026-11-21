/*
 * zpool_health.c - zpool status / list 解析与异步采样
 */

#include "zpool_health.h"

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ZpoolHealth g_health;
static gboolean    g_valid = FALSE;
static gint64      g_last_ms;
static gboolean    g_running = FALSE;

static void on_status(GObject *src, GAsyncResult *res, gpointer data);

static void on_latency(GObject *src, GAsyncResult *res, gpointer data);
static void on_dataset(GObject *src, GAsyncResult *res, gpointer data);
static void on_reqsize(GObject *src, GAsyncResult *res, gpointer data);

static void launch_latency(void)
{
    GError     *err = NULL;
    GSubprocess *p  = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                       "zpool", "iostat", "-w", "1", "1",
                                       NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        g_valid   = TRUE;
        g_running = FALSE;
        return;
    }

    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_latency, NULL);
}

static void on_txg(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL)
        zpool_health_parse_txg(out, &g_health);

    g_free(out);
    g_object_unref(src);

    /* 第六步：数据集级 kstat（逻辑写入 / ZIL），一次 sysctl 链表 */
    if (g_health.pool[0] != '\0') {
        gchar *cmd = g_strdup_printf(
            "n=$(sysctl -N kstat.zfs.%s.dataset 2>/dev/null | grep '\\.nwritten$' | "\
            "xargs sysctl -n 2>/dev/null | awk '{s+=$1} END {print s+0}'); "     \
            "c=$(sysctl -N kstat.zfs.%s.dataset 2>/dev/null | grep '\\.zil_commit_count$' | "\
            "xargs sysctl -n 2>/dev/null | awk '{s+=$1} END {print s+0}'); "     \
            "s=$(sysctl -N kstat.zfs.%s.dataset 2>/dev/null | grep '\\.zil_commit_stall_count$' | "\
            "xargs sysctl -n 2>/dev/null | awk '{s+=$1} END {print s+0}'); "     \
            "t=$(sysctl -N kstat.zfs.%s.dataset 2>/dev/null | grep '\\.zil_itx_count$' | "\
            "xargs sysctl -n 2>/dev/null | awk '{s+=$1} END {print s+0}'); "     \
            "dd=$(sysctl -n kstat.zfs.misc.dmu_tx.dmu_tx_dirty_delay 2>/dev/null); "\
            "th=$(sysctl -n kstat.zfs.misc.dmu_tx.dmu_tx_dirty_throttle 2>/dev/null); "\
            "wl=$(sysctl -n kstat.zfs.misc.dmu_tx.dmu_tx_wrlog_delay 2>/dev/null); "\
            "u=$(zfs get -Hp -o value used %s 2>/dev/null); "                  \
            "echo $n $c $s $t $dd $th $wl $u",
            g_health.pool, g_health.pool, g_health.pool, g_health.pool,
            g_health.pool);

        GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                          "sh", "-c", cmd, NULL);

        g_free(cmd);

        if (p != NULL) {
            g_subprocess_communicate_utf8_async(p, NULL, NULL, on_dataset,
                                                NULL);
            return;
        }
    }

    launch_latency();
}

static void on_dataset(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL) {
        unsigned long long n = 0, c = 0, s = 0, t = 0;
        unsigned long long dd = 0, th = 0, wl = 0, u = 0;

        if (sscanf(out, "%llu %llu %llu %llu %llu %llu %llu %llu", &n, &c,
                   &s, &t, &dd, &th, &wl, &u) >= 8) {
            g_health.ds_written = n;
            g_health.zil_commit = c;
            g_health.zil_stall  = s;
            g_health.zil_itx    = t;
            g_health.dmu_dirty_delay    = dd;
            g_health.dmu_dirty_throttle = th;
            g_health.dmu_wrlog_delay    = wl;
            g_health.used_logical       = u;
        }
    }

    g_free(out);
    g_object_unref(src);
    launch_latency();
}

static void on_latency(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL)
        zpool_health_parse_latency(out, &g_health);

    g_free(out);
    g_object_unref(src);

    /* 第七步：写请求大小分布（zpool iostat -r） */
    GError     *e2 = NULL;
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &e2,
                                      "zpool", "iostat", "-r", "1", "1",
                                      NULL);

    if (p == NULL) {
        if (e2 != NULL)
            g_error_free(e2);
        g_valid   = TRUE;
        g_running = FALSE;
        return;
    }

    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_reqsize, NULL);
}

static void on_reqsize(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL)
        zpool_health_parse_reqsize(out, &g_health);

    g_free(out);
    g_object_unref(src);
    g_valid   = TRUE;
    g_running = FALSE;
}

static void on_vdev(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL)
        zpool_health_parse_vdevs(out, &g_health);

    g_free(out);
    g_object_unref(src);

    /* 第五步：TXG 统计（kstat.zfs.<pool>.txgs） */
    if (g_health.pool[0] == '\0') {
        g_valid = TRUE;
        g_running = FALSE;
        return;
    }

    gchar *name = g_strdup_printf("kstat.zfs.%s.txgs", g_health.pool);
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                      "sysctl", "-n", name, NULL);

    g_free(name);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        g_valid = TRUE;
        g_running = FALSE;
        return;
    }

    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_txg, NULL);
}

static void on_iostat(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL)
        zpool_health_parse_iostat(out, &g_health);

    g_free(out);
    g_object_unref(src);

    /* 第四步：vdev 级吞吐/IOPS */
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                      "zpool", "iostat", "-Hpv", "-l", "-p",
                                      "1", "1", NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        g_valid = TRUE;
        g_running = FALSE;
        return;
    }

    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_vdev, NULL);
}

static void on_list(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL)
        zpool_health_parse_frag(out, &g_health);

    g_free(out);
    g_object_unref(src);

    /* 第三步：队列深度与延迟 */
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                      "zpool", "iostat", "-H", "-l", "-q",
                                      "1", "1", NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        g_valid = TRUE;
        g_running = FALSE;
        return;
    }

    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_iostat, NULL);
}

static void on_status(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    if (out != NULL) {
        ZpoolHealth h;

        memset(&h, 0, sizeof(h));
        h.frag_pct = -1;

        if (zpool_health_parse_status(out, &h)) {
            g_health = h;
            g_health.valid = TRUE;
        }
    }

    g_free(out);
    g_object_unref(src);

    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                      "zpool", "list", "-Hp", "-o",
                                      "name,frag", NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        g_running = FALSE;
        return;
    }

    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_list, NULL);
}

void zpool_health_update(void)
{
    if (g_running)
        return;

    gint64 now = g_get_monotonic_time() / 1000;

    if (g_last_ms != 0 && now - g_last_ms < 15000)
        return;

    g_last_ms = now;

    GError      *err = NULL;
    GSubprocess *p   = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                        "zpool", "status", "-sv", NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        return;
    }

    g_running = TRUE;
    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_status, NULL);
}

const ZpoolHealth *zpool_health_get(void)
{
    return g_valid ? &g_health : NULL;
}
