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

gboolean zpool_health_parse_status(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    int         in_config = 0;
    gboolean    got_pool = FALSE;
    char      **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        const char *l = lines[i];

        while (*l == ' ' || *l == '\t')
            l++;

        if (g_str_has_prefix(l, "pool:")) {
            g_strlcpy(out->pool, g_strstrip((gchar *)l + 5),
                      sizeof(out->pool));
            got_pool = TRUE;
        } else if (g_str_has_prefix(l, "state:")) {
            g_strlcpy(out->state, g_strstrip((gchar *)l + 6),
                      sizeof(out->state));
        } else if (g_str_has_prefix(l, "scan:")) {
            const char *v = g_strstrip((gchar *)l + 5);

            if (v[0] != '\0') {
                g_strlcpy(out->scan, v, sizeof(out->scan));
                out->has_scan = TRUE;

                out->scan_pct[0] = '\0';
                out->scan_eta[0] = '\0';

                if (strstr(v, "in progress") != NULL) {
                    const char *pc = strstr(v, "%");

                    if (pc != NULL) {
                        /* 回退到数字起点 */
                        const char *p = pc;

                        while (p > v && (g_ascii_isdigit(p[-1]) ||
                                         p[-1] == '.'))
                            p--;
                        g_strlcpy(out->scan_pct, p,
                                  MIN(sizeof(out->scan_pct),
                                      (size_t)(pc - p) + 2));
                    }

                    const char *eta = strstr(v, "ETA");

                    if (eta != NULL && eta[3] != '\0') {
                        g_strlcpy(out->scan_eta, eta + 3,
                                  sizeof(out->scan_eta));
                        g_strstrip(out->scan_eta);
                    }
                }
            }
        } else if (g_str_has_prefix(l, "logs")) {
            out->has_slog = TRUE;
        } else if (out->has_slog && out->slog[0] == '\0' &&
                   in_config) {
            /* logs 段下面的第一个缩进设备名 */
            char name[64];

            if (sscanf(l, "%63s", name) == 1 &&
                strcmp(name, "NAME") != 0 && strcmp(name, "logs") != 0)
                g_strlcpy(out->slog, name, sizeof(out->slog));
        } else if (g_str_has_prefix(l, "config:")) {
            in_config = 1;
        } else if (in_config && g_str_has_prefix(l, "errors:")) {
            in_config = 0;
        } else if (in_config) {
            /* config 表：NAME STATE READ WRITE CKSUM，末三列是错误计数 */
            char name[64], state[32];
            unsigned long long r = 0, w = 0, c = 0;

            unsigned long long slow = 0;
            int n = sscanf(l, "%63s %31s %llu %llu %llu %llu", name, state,
                           &r, &w, &c, &slow);

            if (n >= 5) {
                out->read_err += r;
                out->write_err += w;
                out->cksum_err += c;
            }
            if (n == 6)
                out->slow_io += slow;
        }
    }

    g_strfreev(lines);
    return got_pool;
}

gboolean zpool_health_parse_frag(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    char **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;

        /* -Hp 输出：name<tab>frag */
        char name[32];
        int  frag = -1;

        if (sscanf(lines[i], "%31s %d", name, &frag) == 2 && frag >= 0) {
            g_strlcpy(out->pool, name, sizeof(out->pool));
            out->frag_pct = frag;
            g_strfreev(lines);
            return TRUE;
        }
    }

    g_strfreev(lines);
    return FALSE;
}

static double parse_wait_ms(const char *tok)
{
    if (tok == NULL || tok[0] == '\0' || tok[0] == '-')
        return 0.0;

    char  *end = NULL;
    double v   = g_ascii_strtod(tok, &end);

    if (end == NULL)
        return 0.0;

    if (g_str_has_suffix(tok, "us"))
        return v / 1000.0;
    if (g_str_has_suffix(tok, "ms"))
        return v;
    if (g_str_has_suffix(tok, "s"))
        return v * 1000.0;

    return v;
}

gboolean zpool_health_parse_iostat(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    char **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;

        char **f = g_strsplit(lines[i], "\t", -1);
        int    n = g_strv_length(f);

        if (n >= 32) {
            out->total_wait_r_ms = parse_wait_ms(f[7]);
            out->total_wait_w_ms = parse_wait_ms(f[8]);
            out->disk_wait_r_ms  = parse_wait_ms(f[9]);
            out->disk_wait_w_ms  = parse_wait_ms(f[10]);

            out->q_pend  = 0.0;
            out->q_activ = 0.0;

            for (int c = 18; c + 1 < n; c += 2) {
                out->q_pend  += g_ascii_strtod(f[c], NULL);
                out->q_activ += g_ascii_strtod(f[c + 1], NULL);
            }

            out->has_iostat = TRUE;
            g_strfreev(f);
            g_strfreev(lines);
            return TRUE;
        }

        g_strfreev(f);
    }

    g_strfreev(lines);
    return FALSE;
}

int zpool_health_parse_vdevs(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return 0;

    char **lines = g_strsplit(text, "\n", -1);
    int    count = 0;
    int    row   = 0;

    for (int i = 0; lines[i] != NULL && count < 8; i++) {
        if (lines[i][0] == '\0')
            continue;

        char  name[32];
        double rops, wops, rbps, wbps;
        double twr = 0, tww = 0, dwr = 0, dww = 0, swr = 0, sww = 0;
        long long alloc, freeb;
        int   n = sscanf(lines[i],
                         "%31s %lld %lld %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
                         name, &alloc, &freeb, &rops, &wops, &rbps, &wbps,
                         &twr, &tww, &dwr, &dww, &swr, &sww);

        if (n < 7)
            continue;

        if (row++ == 0) {
            g_strlcpy(out->pool, name, sizeof(out->pool));
            continue;   /* 第一行是池汇总 */
        }

        g_strlcpy(out->vdev[count].name, name, sizeof(out->vdev[0].name));
        out->vdev[count].rops = rops;
        out->vdev[count].wops = wops;
        out->vdev[count].rbps = rbps;
        out->vdev[count].wbps = wbps;

        /* -l 的等待时间是纳秒（-p），换算成毫秒 */
        out->vdev[count].tw_r_ms = twr / 1e6;
        out->vdev[count].tw_w_ms = tww / 1e6;
        out->vdev[count].dw_r_ms = dwr / 1e6;
        out->vdev[count].dw_w_ms = dww / 1e6;
        out->vdev[count].sq_r_ms = swr / 1e6;
        out->vdev[count].sq_w_ms = sww / 1e6;
        count++;
    }

    out->vdev_count = count;
    g_strfreev(lines);
    return count;
}

/* 解析 "3.43K" 这种带单位后缀的计数 */
static double parse_count(const char *tok)
{
    if (tok == NULL || tok[0] == '\0' || tok[0] == '-')
        return 0.0;

    char  *end = NULL;
    double v   = g_ascii_strtod(tok, &end);

    if (end != NULL) {
        if (*end == 'K')
            v *= 1000.0;
        else if (*end == 'M')
            v *= 1000000.0;
    }

    return v;
}

gboolean zpool_health_parse_latency(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    char **lines = g_strsplit(text, "\n", -1);

    out->lat_n = 0;

    for (int i = 0; lines[i] != NULL && out->lat_n < 12; i++) {
        const char *l = lines[i];

        /* 桶标签形如 1ns / 100us / 1ms / 1s */
        const char *digits = l;
        char       *end    = NULL;

        (void)g_ascii_strtod(digits, &end);

        if (end == NULL || end == digits)
            continue;
        if (!(g_str_has_prefix(end, "ns") || g_str_has_prefix(end, "us") ||
              g_str_has_prefix(end, "ms") ||
              (g_str_has_prefix(end, "s") && !g_str_has_prefix(end, "sq"))))
            continue;

        char **cols = g_strsplit(l, " ", -1);
        int    n    = 0;

        for (int k = 0; cols[k] != NULL; k++) {
            if (cols[k][0] != '\0')
                cols[n++] = cols[k];
        }
        cols[n] = NULL;

        if (n >= 9) {
            /* total_wait / disk_wait / syncq_wait / asyncq_wait 的读+写 */
            double cnt = 0.0;

            for (int k = 1; k <= 8; k++)
                cnt += parse_count(cols[k]);

            size_t ln = (size_t)(end - digits) + 2;

            g_strlcpy(out->lat[out->lat_n].label, l, ln + 1);
            out->lat[out->lat_n].count = cnt;
            out->lat_n++;
        }

        g_strfreev(cols);
    }

    g_strfreev(lines);
    return out->lat_n > 0;
}

guint64 zpool_health_sum_lines(const char *text)
{
    if (text == NULL)
        return 0;

    guint64 sum  = 0;
    char  **lns = g_strsplit(text, "\n", -1);

    for (int i = 0; lns[i] != NULL; i++) {
        if (lns[i][0] == '\0')
            continue;
        sum += g_ascii_strtoull(lns[i], NULL, 10);
    }

    g_strfreev(lns);
    return sum;
}

/* 解析 "512" / "4K" / "1M" 这类尺寸标签 */
static double parse_size(const char *tok)
{
    char  *end = NULL;
    double v   = g_ascii_strtod(tok, &end);

    if (end != NULL) {
        if (*end == 'K')
            v *= 1024.0;
        else if (*end == 'M')
            v *= 1024.0 * 1024.0;
    }

    return v;
}

gboolean zpool_health_parse_reqsize(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    char **lines = g_strsplit(text, "\n", -1);

    out->req_small = 0.0;
    out->req_mid   = 0.0;
    out->req_large = 0.0;

    for (int i = 0; lines[i] != NULL; i++) {
        char **cols = g_strsplit_set(lines[i], " \t", -1);
        int    n    = 0;

        for (int k = 0; cols[k] != NULL; k++) {
            if (cols[k][0] != '\0')
                cols[n++] = cols[k];
        }
        cols[n] = NULL;

        /* 至少要有尺寸 + sync/async 读写共 9 列 */
        if (n >= 9) {
            double sz = parse_size(cols[0]);

            if (sz >= 512.0) {
                /* 写请求 = sync_write ind/agg + async_write ind/agg */
                double wr = parse_count(cols[3]) + parse_count(cols[4]) +
                            parse_count(cols[7]) + parse_count(cols[8]);

                if (sz <= 4.0 * 1024.0)
                    out->req_small += wr;
                else if (sz <= 64.0 * 1024.0)
                    out->req_mid += wr;
                else
                    out->req_large += wr;
            }
        }

        g_strfreev(cols);
    }

    g_strfreev(lines);
    return out->req_small + out->req_mid + out->req_large > 0.0;
}

gboolean zpool_health_parse_txg(const char *text, ZpoolHealth *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    char **lines = g_strsplit(text, "\n", -1);
    int    txg_col = -1, stime_col = -1;
    int    qtime_col = -1, wtime_col = -1, otime_col = -1;
    gboolean got = FALSE;
    double  samples[16];
    int     n_samples = 0;
    double  last_stime = 0.0;

    for (int i = 0; lines[i] != NULL; i++) {
        char **cols = g_strsplit_set(lines[i], " \t", -1);
        int    n    = 0;

        /* 去掉空 token */
        for (int k = 0; cols[k] != NULL; k++) {
            if (cols[k][0] != '\0')
                cols[n++] = cols[k];
        }
        cols[n] = NULL;

        if (n == 0) {
            g_strfreev(cols);
            continue;
        }

        if (txg_col < 0) {
            /* 表头：找 txg / stime 的列号 */
            for (int k = 0; k < n; k++) {
                if (strcmp(cols[k], "txg") == 0)
                    txg_col = k;
                if (strcmp(cols[k], "stime") == 0)
                    stime_col = k;
                if (strcmp(cols[k], "qtime") == 0)
                    qtime_col = k;
                if (strcmp(cols[k], "wtime") == 0)
                    wtime_col = k;
                if (strcmp(cols[k], "otime") == 0)
                    otime_col = k;
            }
            g_strfreev(cols);
            continue;
        }

        if (stime_col < 0 || n <= stime_col || n <= txg_col) {
            g_strfreev(cols);
            continue;
        }

        guint64 txg   = g_ascii_strtoull(cols[txg_col], NULL, 10);
        guint64 stime = g_ascii_strtoull(cols[stime_col], NULL, 10);

        if (stime > 0) {
            out->txg      = txg;
            out->stime_ms = (double)stime / 1000.0;  /* µs -> ms */
            out->has_txg  = TRUE;
            got = TRUE;

            last_stime = out->stime_ms;

            /* qtime/wtime 与 stime 同为微秒，换算毫秒 */
            if (qtime_col >= 0 && n > qtime_col)
                out->txg_qtime_ms =
                    g_ascii_strtoull(cols[qtime_col], NULL, 10) / 1000.0;
            if (wtime_col >= 0 && n > wtime_col)
                out->txg_wtime_ms =
                    g_ascii_strtoull(cols[wtime_col], NULL, 10) / 1000.0;
            if (otime_col >= 0 && n > otime_col)
                out->txg_otime_ms =
                    g_ascii_strtoull(cols[otime_col], NULL, 10) / 1000.0;

            if (n_samples < 16)
                samples[n_samples++] = last_stime;
            else {
                memmove(samples, samples + 1,
                        sizeof(double) * 15);
                samples[15] = last_stime;
            }
        }

        g_strfreev(cols);
    }

    if (n_samples > 0) {
        double sum = 0.0, mx = 0.0;

        for (int i = 0; i < n_samples; i++) {
            sum += samples[i];
            if (samples[i] > mx)
                mx = samples[i];
        }

        out->txg_samples = n_samples;
        out->txg_avg_ms  = sum / (double)n_samples;
        out->txg_max_ms  = mx;
        out->txg_spike   = (n_samples >= 4 && out->txg_avg_ms > 0.0 &&
                            last_stime > 2.0 * out->txg_avg_ms);
    }

    g_strfreev(lines);
    return got;
}

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





