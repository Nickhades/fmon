/* daemon sidecar history */
#include "history_sidecars.h"
#include "history_store.h"
#include "history_store_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * GPU 1 分钟层：每个 GPU slot 一行，缺失指标写 nan。
 * 单独文件，避免把可变 slot 数量塞进主 1m 表。
 * ------------------------------------------------------------------------- */
typedef struct {
    double usage_sum, temp_sum, mem_sum;
    unsigned usage_n, temp_n, mem_n;
} GpuBucket;

static long long g_gpu_min = -1;
static GpuBucket g_gpu_bucket[FMON_MAX_GPUS];

static const char *history_gpu_path(void)
{
    static char path[512];

    if (path[0] != '\0')
        return path;

    const char *raw = fmon_history_store_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0)
        snprintf(path, sizeof(path), "%.*s_gpu_1m.tsv", (int)(len - 4), raw);
    else
        snprintf(path, sizeof(path), "%s_gpu_1m.tsv", raw);

    return path;
}

static void flush_gpu_bucket(void)
{
    if (g_gpu_min < 0)
        return;

    const char *path = history_gpu_path();
    char dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash != NULL) {
        *slash = '\0';
        history_store_mkdir_p(dir);
    }

    history_store_rotate_if_needed(path);

    FILE *fp = fopen(path, "a");

    if (fp != NULL) {
        for (int slot = 0; slot < FMON_MAX_GPUS; slot++) {
            GpuBucket *b = &g_gpu_bucket[slot];

            if (b->usage_n == 0 && b->temp_n == 0 && b->mem_n == 0)
                continue;

            double usage = (b->usage_n > 0)
                               ? b->usage_sum / (double)b->usage_n
                               : NAN;
            double temp = (b->temp_n > 0)
                              ? b->temp_sum / (double)b->temp_n
                              : NAN;
            double mem = (b->mem_n > 0) ? b->mem_sum / (double)b->mem_n
                                        : NAN;

            (void)fprintf(fp, "%lld\t%d\t%.2f\t%.2f\t%.2f\n",
                          g_gpu_min * 60, slot, usage, temp, mem);
        }

        (void)fclose(fp);
    }

    memset(g_gpu_bucket, 0, sizeof(g_gpu_bucket));
}

void history_gpu_sample(time_t now, const FmonShmData *s)
{
    long long minute = (long long)now / 60;

    if (g_gpu_min < 0)
        g_gpu_min = minute;

    if (minute != g_gpu_min) {
        flush_gpu_bucket();
        g_gpu_min = minute;
    }

    for (int slot = 0; slot < s->gpu_count && slot < FMON_MAX_GPUS; slot++) {
        const FmonGpuInfo *g = &s->gpus[slot];

        if (g->present == 0 || g->load_available == 0)
            continue;

        GpuBucket *b = &g_gpu_bucket[slot];

        if (isfinite(g->usage_pct) && g->usage_pct >= 0.0) {
            b->usage_sum += g->usage_pct;
            b->usage_n++;
        }
        if (isfinite(g->temp_c) && g->temp_c >= 0.0) {
            b->temp_sum += g->temp_c;
            b->temp_n++;
        }
        if (isfinite(g->mem_used_mb) && g->mem_used_mb >= 0.0) {
            b->mem_sum += g->mem_used_mb;
            b->mem_n++;
        }
    }
}

/* ZFS 1 分钟层：zpool 使用率、ARC 命中率、L2ARC、ZFS I/O。 */
typedef struct {
    double zpool_sum, arc_sum, l2_size_sum, l2_hit_sum, rd_sum, wr_sum;
    unsigned zpool_n, arc_n, l2_size_n, l2_hit_n, io_n;
} ZfsBucket;

static long long g_zfs_min = -1;
static ZfsBucket g_zfs_bucket;

static const char *history_zfs_path(void)
{
    static char path[512];

    if (path[0] != '\0')
        return path;

    const char *raw = fmon_history_store_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0)
        snprintf(path, sizeof(path), "%.*s_zfs_1m.tsv", (int)(len - 4), raw);
    else
        snprintf(path, sizeof(path), "%s_zfs_1m.tsv", raw);

    return path;
}

static void flush_zfs_bucket(void)
{
    if (g_zfs_min < 0)
        return;

    ZfsBucket *b = &g_zfs_bucket;

    if (b->zpool_n == 0 && b->arc_n == 0 && b->l2_size_n == 0 &&
        b->l2_hit_n == 0 && b->io_n == 0)
        return;

    const char *path = history_zfs_path();
    char dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash != NULL) {
        *slash = '\0';
        history_store_mkdir_p(dir);
    }

    history_store_rotate_if_needed(path);

    FILE *fp = fopen(path, "a");

    if (fp != NULL) {
        (void)fprintf(fp, "%lld\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%.1f\n",
                      g_zfs_min * 60,
                      b->zpool_n ? b->zpool_sum / (double)b->zpool_n : NAN,
                      b->arc_n ? b->arc_sum / (double)b->arc_n : NAN,
                      b->l2_size_n
                          ? b->l2_size_sum / (double)b->l2_size_n
                          : NAN,
                      b->l2_hit_n ? b->l2_hit_sum / (double)b->l2_hit_n
                                  : NAN,
                      b->io_n ? b->rd_sum / (double)b->io_n : NAN,
                      b->io_n ? b->wr_sum / (double)b->io_n : NAN);
        (void)fclose(fp);
    }

    memset(b, 0, sizeof(*b));
}

void history_zfs_sample(time_t now, const FmonShmData *s)
{
    long long minute = (long long)now / 60;

    if (g_zfs_min < 0)
        g_zfs_min = minute;

    if (minute != g_zfs_min) {
        flush_zfs_bucket();
        g_zfs_min = minute;
    }

    ZfsBucket *b = &g_zfs_bucket;

    if ((s->probe_valid & FMON_PROBE_POOLS) != 0 && s->pool_count > 0 &&
        s->pools[0].available != 0 && isfinite(s->pools[0].util_pct)) {
        b->zpool_sum += s->pools[0].util_pct;
        b->zpool_n++;
    }

    if ((s->probe_valid & FMON_PROBE_ZFS_ARC) != 0 &&
        isfinite(s->zfs_arc_pct)) {
        b->arc_sum += s->zfs_arc_pct;
        b->arc_n++;
    }

    if (s->zfs_l2_size > 0) {
        b->l2_size_sum += (double)s->zfs_l2_size / (1024.0 * 1024.0 * 1024.0);
        b->l2_size_n++;
    }

    if (s->zfs_l2_hits + s->zfs_l2_misses > 0) {
        b->l2_hit_sum += 100.0 * (double)s->zfs_l2_hits /
                         (double)(s->zfs_l2_hits + s->zfs_l2_misses);
        b->l2_hit_n++;
    }

    if (s->history_count > 0) {
        uint32_t last = (s->history_index + FMON_HISTORY_POINTS - 1) %
                        FMON_HISTORY_POINTS;
        double rd = s->zfs_io_read_history[last];
        double wr = s->zfs_io_write_history[last];

        if (isfinite(rd) && isfinite(wr)) {
            b->rd_sum += rd;
            b->wr_sum += wr;
            b->io_n++;
        }
    }
}
