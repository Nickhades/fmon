/*
 * history_store.c - 指标历史持久化实现
 */

#include "history_store.h"
#include "history_sidecars.h"
#include "history_store_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HISTORY_MAX_BYTES  (20UL * 1024UL * 1024UL)

static char g_path[512];
static bool g_path_ready = false;

/* 逐级创建目录（最简单的 mkdir -p） */
void history_store_mkdir_p(const char *path)
{
    char   buf[512];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(buf))
        return;

    snprintf(buf, sizeof(buf), "%s", path);

    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        (void)mkdir(buf, 0755);
        *p = '/';
    }
    (void)mkdir(buf, 0755);
}

const char *fmon_history_store_path(void)
{
    if (g_path_ready)
        return g_path;

    if (geteuid() == 0) {
        snprintf(g_path, sizeof(g_path), "/var/db/fmon/history.tsv");
    } else {
        const char *home = getenv("HOME");

        if (home == NULL || home[0] == '\0')
            home = "/tmp";
        snprintf(g_path, sizeof(g_path),
                 "%s/.local/state/fmon/history.tsv", home);
    }

    g_path_ready = true;
    return g_path;
}

/* 文件超过 20 MiB 时轮转一次 */
void history_store_rotate_if_needed(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return;
    if ((unsigned long)st.st_size < HISTORY_MAX_BYTES)
        return;

    char old[512];

    snprintf(old, sizeof(old), "%s.1", path);
    (void)unlink(old);
    (void)rename(path, old);
}

/* 多分辨率历史：1 分钟 / 5 分钟 / 1 小时。
 * 原始 10 秒文件仍完整保留；聚合文件只保存 avg/min/max 和样本数。 */
typedef struct {
    double   cpu_avg, cpu_min, cpu_max;
    double   mem_avg, mem_min, mem_max;
    double   swap_avg;
    double   net_rx_avg, net_tx_avg;
    double   disk_rd_avg, disk_wr_avg;
    double   temp_avg, temp_min, temp_max;
    double   disk_temp_avg;
    unsigned disk_temp_n;
    unsigned samples;
} MetricRow;

typedef struct {
    long long start_min;
    MetricRow sum;
    unsigned  buckets;
} LayerAgg;

static long long g_bucket_min = -1;
static unsigned  g_bucket_n;
static double    g_cpu_sum, g_cpu_min, g_cpu_max;
static double    g_mem_sum, g_mem_min, g_mem_max;
static double    g_swap_sum;
static double    g_net_rx_sum, g_net_tx_sum;
static double    g_disk_rd_sum, g_disk_wr_sum;
static double    g_temp_sum, g_temp_min, g_temp_max;
static unsigned  g_temp_n;
static double    g_disk_temp_sum;
static unsigned  g_disk_temp_n;

static LayerAgg g_layer_5m;
static LayerAgg g_layer_1h;

static const char *history_layer_path(const char *suffix)
{
    static char paths[3][512];
    static bool ready[3];
    int slot = 0;

    if (strcmp(suffix, "5m") == 0)
        slot = 1;
    else if (strcmp(suffix, "1h") == 0)
        slot = 2;

    if (ready[slot])
        return paths[slot];

    const char *raw = fmon_history_store_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0) {
        snprintf(paths[slot], sizeof(paths[slot]), "%.*s_%s.tsv",
                 (int)(len - 4), raw, suffix);
    } else {
        snprintf(paths[slot], sizeof(paths[slot]), "%s_%s.tsv", raw, suffix);
    }

    ready[slot] = true;
    return paths[slot];
}

static const char *history_1m_store_path(void)
{
    return history_layer_path("1m");
}

static void ensure_parent_dir(const char *path)
{
    char dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash != NULL) {
        *slash = '\0';
        history_store_mkdir_p(dir);
    }
}

static void write_metric_row(const char *path, long long start_min,
                             const MetricRow *r)
{
    ensure_parent_dir(path);
    history_store_rotate_if_needed(path);

    FILE *fp = fopen(path, "a");

    if (fp == NULL)
        return;

    (void)fprintf(fp,
                  "%lld\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t"
                  "%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t"
                  "%.2f\t%u\n",
                  start_min * 60, r->cpu_avg, r->cpu_min, r->cpu_max,
                  r->mem_avg, r->mem_min, r->mem_max, r->swap_avg,
                  r->net_rx_avg, r->net_tx_avg, r->disk_rd_avg,
                  r->disk_wr_avg, r->temp_avg, r->temp_min, r->temp_max,
                  r->disk_temp_avg, r->samples);

    (void)fclose(fp);
}

static void layer_reset(LayerAgg *a)
{
    memset(&a->sum, 0, sizeof(a->sum));
    a->buckets = 0;
}

static void layer_flush(LayerAgg *a, const char *suffix)
{
    if (a->buckets == 0 || a->start_min < 0)
        return;

    MetricRow out = a->sum;

    out.cpu_avg /= (double)a->buckets;
    out.mem_avg /= (double)a->buckets;
    out.swap_avg /= (double)a->buckets;
    out.net_rx_avg /= (double)a->buckets;
    out.net_tx_avg /= (double)a->buckets;
    out.disk_rd_avg /= (double)a->buckets;
    out.disk_wr_avg /= (double)a->buckets;
    out.temp_avg = (out.samples > 0) ? out.temp_avg / (double)out.samples
                                     : NAN;
    out.disk_temp_avg = (out.disk_temp_n > 0)
                            ? out.disk_temp_avg / (double)out.disk_temp_n
                            : NAN;
    out.disk_temp_n = 0;
    out.samples = a->sum.samples;

    write_metric_row(history_layer_path(suffix), a->start_min, &out);
    layer_reset(a);
}

static void layer_add(LayerAgg *a, long long minute, int span,
                      const MetricRow *r)
{
    long long start = (minute / span) * span;

    if (a->start_min < 0)
        a->start_min = start;

    if (start != a->start_min) {
        layer_flush(a, span == 5 ? "5m" : "1h");
        a->start_min = start;
        layer_reset(a);
    }

    if (a->buckets == 0) {
        a->sum = *r;
        a->sum.temp_avg = 0.0;
        a->sum.samples = 0;
    } else {
        a->sum.cpu_avg += r->cpu_avg;
        a->sum.mem_avg += r->mem_avg;
        a->sum.swap_avg += r->swap_avg;
        a->sum.net_rx_avg += r->net_rx_avg;
        a->sum.net_tx_avg += r->net_tx_avg;
        a->sum.disk_rd_avg += r->disk_rd_avg;
        a->sum.disk_wr_avg += r->disk_wr_avg;

        if (isfinite(r->disk_temp_avg) && r->disk_temp_n > 0) {
            a->sum.disk_temp_avg += r->disk_temp_avg * (double)r->disk_temp_n;
            a->sum.disk_temp_n += r->disk_temp_n;
        }

        if (r->cpu_min < a->sum.cpu_min)
            a->sum.cpu_min = r->cpu_min;
        if (r->cpu_max > a->sum.cpu_max)
            a->sum.cpu_max = r->cpu_max;
        if (r->mem_min < a->sum.mem_min)
            a->sum.mem_min = r->mem_min;
        if (r->mem_max > a->sum.mem_max)
            a->sum.mem_max = r->mem_max;

        if (isfinite(r->temp_min)) {
            if (!isfinite(a->sum.temp_min) || r->temp_min < a->sum.temp_min)
                a->sum.temp_min = r->temp_min;
            if (!isfinite(a->sum.temp_max) || r->temp_max > a->sum.temp_max)
                a->sum.temp_max = r->temp_max;
        }
    }

    if (isfinite(r->temp_avg) && r->samples > 0)
        a->sum.temp_avg += r->temp_avg * (double)r->samples;

    a->sum.samples += r->samples;
    a->buckets++;
}

static void flush_1m_bucket(void)
{
    if (g_bucket_n == 0 || g_bucket_min < 0)
        return;

    MetricRow r = { 0 };

    r.cpu_avg = g_cpu_sum / (double)g_bucket_n;
    r.cpu_min = g_cpu_min;
    r.cpu_max = g_cpu_max;
    r.mem_avg = g_mem_sum / (double)g_bucket_n;
    r.mem_min = g_mem_min;
    r.mem_max = g_mem_max;
    r.swap_avg = g_swap_sum / (double)g_bucket_n;
    r.net_rx_avg = g_net_rx_sum / (double)g_bucket_n;
    r.net_tx_avg = g_net_tx_sum / (double)g_bucket_n;
    r.disk_rd_avg = g_disk_rd_sum / (double)g_bucket_n;
    r.disk_wr_avg = g_disk_wr_sum / (double)g_bucket_n;
    r.temp_avg = (g_temp_n > 0) ? g_temp_sum / (double)g_temp_n : NAN;
    r.temp_min = (g_temp_n > 0) ? g_temp_min : NAN;
    r.temp_max = (g_temp_n > 0) ? g_temp_max : NAN;
    r.disk_temp_avg = (g_disk_temp_n > 0)
                          ? g_disk_temp_sum / (double)g_disk_temp_n
                          : NAN;
    r.disk_temp_n = (g_disk_temp_n > 0) ? 1 : 0;
    r.samples = g_bucket_n;

    write_metric_row(history_1m_store_path(), g_bucket_min, &r);

    layer_add(&g_layer_5m, g_bucket_min, 5, &r);
    layer_add(&g_layer_1h, g_bucket_min, 60, &r);

    /* 清空 1 分钟累加器。 */
    g_bucket_n = 0;
    g_cpu_sum = g_cpu_min = g_cpu_max = 0.0;
    g_mem_sum = g_mem_min = g_mem_max = 0.0;
    g_swap_sum = 0.0;
    g_net_rx_sum = g_net_tx_sum = 0.0;
    g_disk_rd_sum = g_disk_wr_sum = 0.0;
    g_temp_sum = g_temp_min = g_temp_max = 0.0;
    g_temp_n = 0;
    g_disk_temp_sum = 0.0;
    g_disk_temp_n = 0;
}

static void aggregate_1m_sample(time_t now, const FmonShmData *s)
{
    long long minute = (long long)now / 60;

    if (g_bucket_min < 0)
        g_bucket_min = minute;

    if (minute != g_bucket_min) {
        flush_1m_bucket();
        g_bucket_min = minute;
    }

    double cpu = s->cpu_usage_pct;
    double mem = s->mem_used_pct;

    if (!isfinite(cpu) || !isfinite(mem))
        return;

    if (g_bucket_n == 0) {
        g_cpu_sum = g_cpu_min = g_cpu_max = cpu;
        g_mem_sum = g_mem_min = g_mem_max = mem;
    } else {
        g_cpu_sum += cpu;
        g_mem_sum += mem;
        if (cpu < g_cpu_min)
            g_cpu_min = cpu;
        if (cpu > g_cpu_max)
            g_cpu_max = cpu;
        if (mem < g_mem_min)
            g_mem_min = mem;
        if (mem > g_mem_max)
            g_mem_max = mem;
    }

    if (isfinite(s->swap_used_pct))
        g_swap_sum += s->swap_used_pct;
    if (isfinite(s->net_rx_kbps))
        g_net_rx_sum += s->net_rx_kbps;
    if (isfinite(s->net_tx_kbps))
        g_net_tx_sum += s->net_tx_kbps;
    if (isfinite(s->disk_read_kbps))
        g_disk_rd_sum += s->disk_read_kbps;
    if (isfinite(s->disk_write_kbps))
        g_disk_wr_sum += s->disk_write_kbps;

    double disk_temp = NAN;

    for (uint32_t i = 0; i < s->disk_count && i < FMON_MAX_DISKS; i++) {
        if (s->disks[i].smart_valid != 0 &&
            isfinite(s->disks[i].temp_c) && s->disks[i].temp_c > 0.0) {
            if (!isfinite(disk_temp) || s->disks[i].temp_c > disk_temp)
                disk_temp = s->disks[i].temp_c;
        }
    }

    if (isfinite(disk_temp)) {
        g_disk_temp_sum += disk_temp;
        g_disk_temp_n++;
    }

    if (isfinite(s->cpu_temp_c)) {
        if (g_temp_n == 0) {
            g_temp_min = g_temp_max = s->cpu_temp_c;
        } else {
            if (s->cpu_temp_c < g_temp_min)
                g_temp_min = s->cpu_temp_c;
            if (s->cpu_temp_c > g_temp_max)
                g_temp_max = s->cpu_temp_c;
        }
        g_temp_sum += s->cpu_temp_c;
        g_temp_n++;
    }

    g_bucket_n++;
}

void fmon_history_store_sample(const FmonShmData *s)
{
    if (s == NULL)
        return;

    const char *path = fmon_history_store_path();
    char        dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash != NULL) {
        *slash = '\0';
        history_store_mkdir_p(dir);
    }

    history_store_rotate_if_needed(path);

    FILE *fp = fopen(path, "a");

    if (fp == NULL)
        return;

    time_t now = time(NULL);

    (void)fprintf(fp, "%lld\t%.2f\t%.2f\t%.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
                  (long long)now, s->cpu_usage_pct, s->mem_used_pct,
                  s->swap_used_pct, s->net_rx_kbps, s->net_tx_kbps,
                  s->disk_read_kbps, s->disk_write_kbps, s->cpu_temp_c);

    (void)fclose(fp);

    aggregate_1m_sample(now, s);
    history_gpu_sample(now, s);
    history_zfs_sample(now, s);
}
