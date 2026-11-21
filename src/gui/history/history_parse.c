/*
 * history_parse.c - history TSV 行解析与分桶
 */

#include "history_parse.h"

#include "history/history_provider.h"

#include <math.h>
#include <stdio.h>
#include <string.h>



static const int k_value_index[FMON_HIST_COUNT] = {
    0, 3, 6, 7, 8, 9, 10, 11, 14
};
static const int k_min_index[FMON_HIST_COUNT] = {
    1, 4, -1, -1, -1, -1, -1, 12, -1
};
static const int k_max_index[FMON_HIST_COUNT] = {
    2, 5, -1, -1, -1, -1, -1, 13, -1
};

double   s_values[FMON_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
double   s_min[FMON_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
double   s_max[FMON_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
uint64_t s_time_us[FMON_HISTORY_MAX_POINTS];
FmonHistorySeries s_series[FMON_HIST_COUNT];
uint32_t s_count;
uint32_t s_interval_s;
bool     s_loaded;

static bool parse_row(const char *line, HistoryRow *row)
{
    memset(row, 0, sizeof(*row));

    long long ts;
    double    a, b, c, d, e, f, g, h, i, j, k, l, m, n, o;
    unsigned  samples = 0;

    int got = sscanf(line,
                     "%lld\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t"
                     "%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%u",
                     &ts, &a, &b, &c, &d, &e, &f, &g, &h, &i, &j, &k,
                     &l, &m, &n, &o, &samples);

    if (got == 17 && ts > 0 && ts < 10000000000LL) {
        row->ts = ts;
        row->v[0] = a;  row->v[1] = b;  row->v[2] = c;
        row->v[3] = d;  row->v[4] = e;  row->v[5] = f;
        row->v[6] = g;
        row->v[7] = h;  row->v[8] = i;
        row->v[9] = j;  row->v[10] = k;
        row->v[11] = l; row->v[12] = m; row->v[13] = n;
        row->v[14] = o;
        row->samples = samples;
        return true;
    }

    /* 旧 16 列：14 个 double + samples，没有 disk_temp。 */
    samples = 0;
    got = sscanf(line,
                 "%lld\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t"
                 "%lf\t%lf\t%lf\t%lf\t%lf\t%u",
                 &ts, &a, &b, &c, &d, &e, &f, &g, &h, &i, &j, &k,
                 &l, &m, &n, &samples);

    if (got == 16 && ts > 0 && ts < 10000000000LL) {
        row->ts = ts;
        row->v[0] = a;  row->v[1] = b;  row->v[2] = c;
        row->v[3] = d;  row->v[4] = e;  row->v[5] = f;
        row->v[6] = g;
        row->v[7] = h;  row->v[8] = i;
        row->v[9] = j;  row->v[10] = k;
        row->v[11] = l; row->v[12] = m; row->v[13] = n;
        row->v[14] = NAN;
        row->samples = samples;
        return true;
    }

    /* 最旧 7 列：CPU/内存 avg/min/max。 */
    samples = 0;
    got = sscanf(line, "%lld\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf",
                 &ts, &a, &b, &c, &d, &e, &f);

    if (got == 7 && ts > 0 && ts < 10000000000LL) {
        row->ts = ts;
        row->v[0] = a;  row->v[1] = b;  row->v[2] = c;
        row->v[3] = d;  row->v[4] = e;  row->v[5] = f;
        for (int q = 6; q < 15; q++)
            row->v[q] = NAN;
        row->old_format = true;
        row->samples = 1;
        return true;
    }

    return false;
}

static bool parse_raw_row(const char *line, HistoryRow *row)
{
    memset(row, 0, sizeof(*row));

    long long ts;
    double    cpu, mem, swap, rx, tx, drd, dwr, temp;

    if (sscanf(line, "%lld\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf",
               &ts, &cpu, &mem, &swap, &rx, &tx, &drd, &dwr, &temp) != 9)
        return false;

    if (ts <= 0 || ts >= 10000000000LL)
        return false;

    row->ts = ts;
    row->v[0] = row->v[1] = row->v[2] = cpu;
    row->v[3] = row->v[4] = row->v[5] = mem;
    row->v[6] = swap;
    row->v[7] = rx;
    row->v[8] = tx;
    row->v[9] = drd;
    row->v[10] = dwr;
    row->v[11] = row->v[12] = row->v[13] = temp;
    row->v[14] = NAN;
    row->samples = 1;

    return true;
}

GArray *history_read_rows(const char *path, gint64 cutoff, bool raw,
                         bool *old_format)
{
    gchar *content = NULL;
    gsize  len = 0;

    if (!g_file_get_contents(path, &content, &len, NULL) || len == 0) {
        g_free(content);
        return NULL;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    GArray *rows = g_array_new(FALSE, FALSE, sizeof(HistoryRow));

    for (int i = 0; lines[i] != NULL; i++) {
        HistoryRow row;

        bool ok = raw ? parse_raw_row(lines[i], &row)
                      : parse_row(lines[i], &row);

        if (!ok || row.ts < cutoff)
            continue;

        if (!raw && row.old_format && old_format != NULL)
            *old_format = true;

        g_array_append_val(rows, row);
    }

    g_strfreev(lines);
    return rows;
}

void history_bucket_rows(const GArray *rows, uint32_t base_interval_s)
{
    guint n = rows->len;
    guint bucket = (n + FMON_HISTORY_MAX_POINTS - 1) / FMON_HISTORY_MAX_POINTS;

    if (bucket < 1)
        bucket = 1;

    for (int m = 0; m < FMON_HIST_COUNT; m++) {
        for (guint i = 0; i < FMON_HISTORY_MAX_POINTS; i++) {
            s_values[m][i] = NAN;
            s_min[m][i] = NAN;
            s_max[m][i] = NAN;
        }
    }

    s_count = 0;

    for (guint i = 0; i < n && s_count < FMON_HISTORY_MAX_POINTS;
         i += bucket) {
        guint end = MIN(i + bucket, n);
        guint cnt = 0;
        double sum[FMON_HIST_COUNT] = { 0 };
        double mn[FMON_HIST_COUNT];
        double mx[FMON_HIST_COUNT];
        guint valid[FMON_HIST_COUNT] = { 0 };
        long long last_ts = 0;

        for (int m = 0; m < FMON_HIST_COUNT; m++) {
            mn[m] = INFINITY;
            mx[m] = -INFINITY;
        }

        for (guint r = i; r < end; r++) {
            const HistoryRow *row = &g_array_index(rows, HistoryRow, r);

            last_ts = row->ts;
            cnt++;

            for (int m = 0; m < FMON_HIST_COUNT; m++) {
                double v = row->v[k_value_index[m]];

                if (!isfinite(v))
                    continue;
                sum[m] += v;
                valid[m]++;

                int mi = k_min_index[m];
                int ma = k_max_index[m];

                if (mi >= 0 && isfinite(row->v[mi]) &&
                    row->v[mi] < mn[m])
                    mn[m] = row->v[mi];
                if (ma >= 0 && isfinite(row->v[ma]) &&
                    row->v[ma] > mx[m])
                    mx[m] = row->v[ma];
            }
        }

        if (cnt == 0)
            continue;

        for (int m = 0; m < FMON_HIST_COUNT; m++) {
            if (valid[m] == 0)
                continue;

            s_values[m][s_count] = sum[m] / (double)valid[m];
            s_min[m][s_count] =
                (k_min_index[m] >= 0 && isfinite(mn[m])) ? mn[m] : NAN;
            s_max[m][s_count] =
                (k_max_index[m] >= 0 && isfinite(mx[m])) ? mx[m] : NAN;
        }

        s_time_us[s_count] =
            (last_ts > 0 && last_ts < 10000000000LL)
                ? (uint64_t)last_ts * 1000000ULL
                : 0;
        s_count++;
    }

    s_interval_s = bucket * base_interval_s;

    for (int m = 0; m < FMON_HIST_COUNT; m++) {
        s_series[m].values = s_values[m];
        s_series[m].min = s_min[m];
        s_series[m].max = s_max[m];
        s_series[m].available = false;

        for (uint32_t i = 0; i < s_count; i++) {
            if (isfinite(s_values[m][i])) {
                s_series[m].available = true;
                break;
            }
        }
    }
}
