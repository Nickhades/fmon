#include "history/history_provider.h"

#include "history/history_parse.h"

#include "fmon_shm.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *history_path(void)
{
    const char *override = g_getenv("FMON_HISTORY_FILE");

    if (override != NULL && override[0] != '\0')
        return override;

    if (g_file_test("/var/db/fmon/history.tsv", G_FILE_TEST_EXISTS))
        return "/var/db/fmon/history.tsv";

    static char path[512];
    const char *home = g_get_home_dir();

    g_snprintf(path, sizeof(path), "%s/.local/state/fmon/history.tsv",
               (home != NULL) ? home : "/tmp");
    return path;
}

static const char *history_layer_path(const char *suffix)
{
    static char paths[3][512];
    static bool ready[3];
    int slot = (strcmp(suffix, "5m") == 0) ? 1
               : (strcmp(suffix, "1h") == 0) ? 2
                                             : 0;

    if (ready[slot])
        return paths[slot];

    const char *raw = history_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0)
        g_snprintf(paths[slot], sizeof(paths[slot]), "%.*s_%s.tsv",
                   (int)(len - 4), raw, suffix);
    else
        g_snprintf(paths[slot], sizeof(paths[slot]), "%s_%s.tsv", raw,
                   suffix);

    ready[slot] = true;
    return paths[slot];
}

static const char *history_1m_path(void)
{
    return history_layer_path("1m");
}

typedef struct {
    const char *path;
    uint32_t    interval_s;
    bool        raw;
} LayerCandidate;

static GArray *load_best_layer(gint64 range_secs, gint64 cutoff,
                               uint32_t *base_interval)
{
    LayerCandidate candidates[4];
    int count = 0;

    if (range_secs <= 86400) {
        candidates[count++] = (LayerCandidate){ history_1m_path(), 60, FALSE };
    } else if (range_secs <= 7 * 86400) {
        candidates[count++] = (LayerCandidate){ history_layer_path("5m"), 300, FALSE };
        candidates[count++] = (LayerCandidate){ history_1m_path(), 60, FALSE };
    } else {
        candidates[count++] = (LayerCandidate){ history_layer_path("1h"), 3600, FALSE };
        candidates[count++] = (LayerCandidate){ history_layer_path("5m"), 300, FALSE };
        candidates[count++] = (LayerCandidate){ history_1m_path(), 60, FALSE };
    }

    candidates[count++] = (LayerCandidate){ history_path(), 10, TRUE };

    for (int i = 0; i < count; i++) {
        /* 30 天回退到 10 秒原始文件会占用大量内存；没有 1h/5m/1m
         * 层时宁可不显示长历史，也不一次性加载 25 万行。 */
        if (candidates[i].raw && range_secs > 7 * 86400)
            continue;

        bool old_format = false;
        GArray *rows = history_read_rows(candidates[i].path, cutoff,
                                 candidates[i].raw, &old_format);

        if (rows != NULL && rows->len > 0 && !old_format) {
            *base_interval = candidates[i].interval_s;
            return rows;
        }

        if (rows != NULL)
            g_array_free(rows, TRUE);
    }

    return NULL;
}

bool fmon_history_load(gint64 range_secs)
{
    fmon_history_unload();

    gint64 cutoff = (gint64)time(NULL) - range_secs;
    uint32_t base_interval = 0;
    GArray *rows = load_best_layer(range_secs, cutoff, &base_interval);

    if (rows == NULL || rows->len == 0) {
        if (rows != NULL)
            g_array_free(rows, TRUE);
        return false;
    }

    history_bucket_rows(rows, base_interval);
    g_array_free(rows, TRUE);

    s_loaded = s_count > 0;
    return s_loaded;
}

typedef struct {
    long long ts;
    double    v[FMON_ZFS_HIST_COUNT];
} ZfsRow;

static double s_zfs_values[FMON_ZFS_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
static uint64_t s_zfs_time_us[FMON_HISTORY_MAX_POINTS];
static FmonHistorySeries s_zfs_series[FMON_ZFS_HIST_COUNT];
static uint32_t s_zfs_count;
static bool     s_zfs_loaded;

static const char *history_zfs_path(void)
{
    static char path[512];

    if (path[0] != '\0')
        return path;

    const char *raw = history_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0)
        g_snprintf(path, sizeof(path), "%.*s_zfs_1m.tsv", (int)(len - 4), raw);
    else
        g_snprintf(path, sizeof(path), "%s_zfs_1m.tsv", raw);

    return path;
}

bool fmon_zfs_history_load(gint64 range_secs)
{
    fmon_zfs_history_unload();

    gchar *content = NULL;
    gsize  len = 0;

    if (!g_file_get_contents(history_zfs_path(), &content, &len, NULL) ||
        len == 0) {
        g_free(content);
        return false;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    gint64 cutoff = (gint64)time(NULL) - range_secs;
    GArray *rows = g_array_new(FALSE, FALSE, sizeof(ZfsRow));

    for (int i = 0; lines[i] != NULL; i++) {
        ZfsRow r;
        long long ts;
        double a, b, c, d, e, f;

        if (sscanf(lines[i], "%lld\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf", &ts,
                   &a, &b, &c, &d, &e, &f) != 7)
            continue;
        if (ts <= 0 || ts < cutoff)
            continue;

        r.ts = ts;
        r.v[0] = a;
        r.v[1] = b;
        r.v[2] = c;
        r.v[3] = d;
        r.v[4] = e;
        r.v[5] = f;
        g_array_append_val(rows, r);
    }

    g_strfreev(lines);

    guint total = rows->len;

    if (total == 0) {
        g_array_free(rows, TRUE);
        return false;
    }

    guint bucket = (total + FMON_HISTORY_MAX_POINTS - 1) /
                   FMON_HISTORY_MAX_POINTS;

    if (bucket < 1)
        bucket = 1;

    uint32_t out = 0;

    for (guint i = 0; i < total && out < FMON_HISTORY_MAX_POINTS;
         i += bucket) {
        guint end = MIN(i + bucket, total);
        double sum[FMON_ZFS_HIST_COUNT] = { 0 };
        unsigned valid[FMON_ZFS_HIST_COUNT] = { 0 };
        long long last_ts = 0;

        for (guint j = i; j < end; j++) {
            const ZfsRow *r = &g_array_index(rows, ZfsRow, j);

            last_ts = r->ts;
            for (int m = 0; m < FMON_ZFS_HIST_COUNT; m++) {
                if (isfinite(r->v[m])) {
                    sum[m] += r->v[m];
                    valid[m]++;
                }
            }
        }

        for (int m = 0; m < FMON_ZFS_HIST_COUNT; m++) {
            s_zfs_values[m][out] =
                (valid[m] > 0) ? sum[m] / (double)valid[m] : NAN;
        }
        s_zfs_time_us[out] = (uint64_t)last_ts * 1000000ULL;
        out++;
    }

    g_array_free(rows, TRUE);
    s_zfs_count = out;
    s_zfs_loaded = out > 0;

    for (int m = 0; m < FMON_ZFS_HIST_COUNT; m++) {
        s_zfs_series[m].values = s_zfs_values[m];
        s_zfs_series[m].min = NULL;
        s_zfs_series[m].max = NULL;
        s_zfs_series[m].available = false;

        for (uint32_t i = 0; i < out; i++) {
            if (isfinite(s_zfs_values[m][i])) {
                s_zfs_series[m].available = true;
                break;
            }
        }
    }

    return s_zfs_loaded;
}

void fmon_zfs_history_unload(void)
{
    s_zfs_loaded = false;
    s_zfs_count = 0;
}

const FmonHistorySeries *fmon_zfs_history_series(FmonZfsHistoryMetric metric)
{
    if (!s_zfs_loaded || metric < 0 || metric >= FMON_ZFS_HIST_COUNT)
        return NULL;

    return &s_zfs_series[metric];
}

const uint64_t *fmon_zfs_history_time_us(void)
{
    return s_zfs_loaded ? s_zfs_time_us : NULL;
}

uint32_t fmon_zfs_history_count(void)
{
    return s_zfs_loaded ? s_zfs_count : 0;
}

typedef struct {
    long long ts;
    int       slot;
    double    usage;
    double    temp;
    double    mem;
} GpuHistoryRow;

static double s_gpu_values[FMON_MAX_GPUS][FMON_GPU_HIST_COUNT]
                          [FMON_HISTORY_MAX_POINTS];
static uint64_t s_gpu_time_us[FMON_MAX_GPUS][FMON_HISTORY_MAX_POINTS];
static FmonHistorySeries s_gpu_series[FMON_MAX_GPUS][FMON_GPU_HIST_COUNT];
static uint32_t s_gpu_count[FMON_MAX_GPUS];
static bool     s_gpu_loaded;

static const char *history_gpu_path(void)
{
    static char path[512];

    if (path[0] != '\0')
        return path;

    const char *raw = history_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0)
        g_snprintf(path, sizeof(path), "%.*s_gpu_1m.tsv", (int)(len - 4), raw);
    else
        g_snprintf(path, sizeof(path), "%s_gpu_1m.tsv", raw);

    return path;
}

bool fmon_gpu_history_load(gint64 range_secs)
{
    fmon_gpu_history_unload();

    gchar *content = NULL;
    gsize  len = 0;

    if (!g_file_get_contents(history_gpu_path(), &content, &len, NULL) ||
        len == 0) {
        g_free(content);
        return false;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    gint64 cutoff = (gint64)time(NULL) - range_secs;
    GArray *slots[FMON_MAX_GPUS];

    for (int i = 0; i < FMON_MAX_GPUS; i++)
        slots[i] = g_array_new(FALSE, FALSE, sizeof(GpuHistoryRow));

    for (int i = 0; lines[i] != NULL; i++) {
        long long ts;
        int slot;
        double usage, temp, mem;

        if (sscanf(lines[i], "%lld\t%d\t%lf\t%lf\t%lf", &ts, &slot,
                   &usage, &temp, &mem) != 5)
            continue;
        if (ts <= 0 || ts < cutoff || slot < 0 || slot >= FMON_MAX_GPUS)
            continue;

        GpuHistoryRow r = { ts, slot, usage, temp, mem };

        g_array_append_val(slots[slot], r);
    }

    g_strfreev(lines);

    bool any = false;

    for (int slot = 0; slot < FMON_MAX_GPUS; slot++) {
        guint total = slots[slot]->len;

        if (total == 0)
            continue;

        guint bucket = (total + FMON_HISTORY_MAX_POINTS - 1) /
                       FMON_HISTORY_MAX_POINTS;

        if (bucket < 1)
            bucket = 1;

        uint32_t out = 0;

        for (guint i = 0; i < total && out < FMON_HISTORY_MAX_POINTS;
             i += bucket) {
            guint end = MIN(i + bucket, total);
            double sum[FMON_GPU_HIST_COUNT] = { 0 };
            unsigned valid[FMON_GPU_HIST_COUNT] = { 0 };
            long long last_ts = 0;

            for (guint j = i; j < end; j++) {
                const GpuHistoryRow *r =
                    &g_array_index(slots[slot], GpuHistoryRow, j);

                last_ts = r->ts;
                if (isfinite(r->usage)) {
                    sum[0] += r->usage;
                    valid[0]++;
                }
                if (isfinite(r->temp)) {
                    sum[1] += r->temp;
                    valid[1]++;
                }
                if (isfinite(r->mem)) {
                    sum[2] += r->mem;
                    valid[2]++;
                }
            }

            for (int m = 0; m < FMON_GPU_HIST_COUNT; m++) {
                s_gpu_values[slot][m][out] =
                    (valid[m] > 0) ? sum[m] / (double)valid[m] : NAN;
            }
            s_gpu_time_us[slot][out] = (uint64_t)last_ts * 1000000ULL;
            out++;
        }

        s_gpu_count[slot] = out;

        for (int m = 0; m < FMON_GPU_HIST_COUNT; m++) {
            s_gpu_series[slot][m].values = s_gpu_values[slot][m];
            s_gpu_series[slot][m].min = NULL;
            s_gpu_series[slot][m].max = NULL;
            s_gpu_series[slot][m].available = false;

            for (uint32_t k = 0; k < out; k++) {
                if (isfinite(s_gpu_values[slot][m][k])) {
                    s_gpu_series[slot][m].available = true;
                    break;
                }
            }
        }

        any = true;
    }

    for (int i = 0; i < FMON_MAX_GPUS; i++)
        g_array_free(slots[i], TRUE);

    s_gpu_loaded = any;
    return any;
}

void fmon_gpu_history_unload(void)
{
    s_gpu_loaded = false;
    memset(s_gpu_count, 0, sizeof(s_gpu_count));
}

const FmonHistorySeries *fmon_gpu_history_series(int slot,
                                                 FmonGpuHistoryMetric metric)
{
    if (!s_gpu_loaded || slot < 0 || slot >= FMON_MAX_GPUS ||
        metric < 0 || metric >= FMON_GPU_HIST_COUNT ||
        s_gpu_count[slot] == 0)
        return NULL;

    return &s_gpu_series[slot][metric];
}

const uint64_t *fmon_gpu_history_time_us(int slot)
{
    if (!s_gpu_loaded || slot < 0 || slot >= FMON_MAX_GPUS ||
        s_gpu_count[slot] == 0)
        return NULL;

    return s_gpu_time_us[slot];
}

uint32_t fmon_gpu_history_count(int slot)
{
    if (!s_gpu_loaded || slot < 0 || slot >= FMON_MAX_GPUS)
        return 0;

    return s_gpu_count[slot];
}

void fmon_history_unload(void)
{
    s_loaded = false;
    s_count = 0;
    s_interval_s = 0;
}

bool fmon_history_loaded(void)
{
    return s_loaded;
}

const FmonHistorySeries *fmon_history_series(FmonHistoryMetric metric)
{
    if (!s_loaded || metric < 0 || metric >= FMON_HIST_COUNT)
        return NULL;

    return &s_series[metric];
}

const uint64_t *fmon_history_time_us(void)
{
    return s_loaded ? s_time_us : NULL;
}

uint32_t fmon_history_count(void)
{
    return s_loaded ? s_count : 0;
}

uint32_t fmon_history_interval_s(void)
{
    return s_loaded ? s_interval_s : 0;
}
