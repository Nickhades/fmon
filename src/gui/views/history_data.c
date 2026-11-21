/*
 * history_data.c - 历史采样缓冲与读取
 */

#include "history_internal.h"

#include "gui_util.h"

#include <math.h>
#include <string.h>
#include <time.h>

double   g_cpu[HIST_POINTS];
double   g_cpu_min[HIST_POINTS];
double   g_cpu_max[HIST_POINTS];
double   g_mem[HIST_POINTS];
double   g_mem_min[HIST_POINTS];
double   g_mem_max[HIST_POINTS];
uint64_t g_time_us[HIST_POINTS];

static const char *history_path(void)
{
    static char path[512];

    if (path[0] != '\0')
        return path;

    /* 本地测试/演示用：允许把历史页指向一个合成 TSV。安装运行时不设置
     * 该变量，仍然优先读取 /var/db/fmon/history.tsv。 */
    const char *override = g_getenv("FMON_HISTORY_FILE");

    if (override != NULL && override[0] != '\0') {
        g_strlcpy(path, override, sizeof(path));
        return path;
    }

    if (g_file_test("/var/db/fmon/history.tsv", G_FILE_TEST_EXISTS)) {
        g_strlcpy(path, "/var/db/fmon/history.tsv", sizeof(path));
    } else {
        const char *home = g_get_home_dir();

        g_snprintf(path, sizeof(path), "%s/.local/state/fmon/history.tsv",
                   (home != NULL) ? home : "/tmp");
    }

    return path;
}

static const char *history_1m_path(void)
{
    static char path[512];

    if (path[0] != '\0')
        return path;

    const char *raw = history_path();
    size_t      len = strlen(raw);

    if (len > 4 && strcmp(raw + len - 4, ".tsv") == 0)
        g_snprintf(path, sizeof(path), "%.*s_1m.tsv", (int)(len - 4), raw);
    else
        g_snprintf(path, sizeof(path), "%s_1m.tsv", raw);

    return path;
}

/* 读取 history_1m.tsv（CPU/内存 1 分钟 avg/min/max）。 */
static guint load_history_1m(gint64 range_secs, const HistoryOut *out,
                             gint64 *out_interval_secs, gdouble *avg_cpu,
                             gdouble *max_cpu, gdouble *avg_mem)
{
    if (out == NULL || out->cap == 0)
        return 0;

    gchar *content = NULL;
    gsize  len     = 0;

    if (!g_file_get_contents(history_1m_path(), &content, &len, NULL) ||
        len == 0) {
        g_free(content);
        return 0;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    gint   total  = (gint)g_strv_length(lines);
    gint64 cutoff = (gint64)time(NULL) - range_secs;
    gint64 n_in   = 0;

    for (gint i = 0; i < total; i++) {
        long long ts;

        if (sscanf(lines[i], "%lld", &ts) == 1 && ts >= cutoff)
            n_in++;
    }

    if (n_in <= 0) {
        g_strfreev(lines);
        return 0;
    }

    gint64 *times = g_new0(gint64, (gsize)n_in);
    double *ca    = g_new0(double, (gsize)n_in);
    double *cmin  = g_new0(double, (gsize)n_in);
    double *cmax  = g_new0(double, (gsize)n_in);
    double *ma    = g_new0(double, (gsize)n_in);
    double *mmin  = g_new0(double, (gsize)n_in);
    double *mmax  = g_new0(double, (gsize)n_in);
    gint    got   = 0;
    double  sumc = 0.0, summ = 0.0, peak = 0.0;

    for (gint i = 0; i < total && got < (gint)n_in; i++) {
        long long ts;
        double    c_avg, c_min, c_max, m_avg, m_min, m_max;

        if (sscanf(lines[i], "%lld\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf", &ts,
                   &c_avg, &c_min, &c_max, &m_avg, &m_min, &m_max) != 7)
            continue;
        if (ts < cutoff || !isfinite(c_avg) || !isfinite(m_avg))
            continue;

        times[got] = ts;
        ca[got] = c_avg;
        cmin[got] = c_min;
        cmax[got] = c_max;
        ma[got] = m_avg;
        mmin[got] = m_min;
        mmax[got] = m_max;
        sumc += c_avg;
        summ += m_avg;
        if (c_max > peak)
            peak = c_max;
        got++;
    }

    g_strfreev(lines);

    if (got <= 0) {
        g_free(times);
        g_free(ca);
        g_free(cmin);
        g_free(cmax);
        g_free(ma);
        g_free(mmin);
        g_free(mmax);
        return 0;
    }

    /* 金字塔尚未覆盖完整 24h 时退回原始 10 秒文件，避免只显示
     * 守护进程刚启动后的一小段数据。 */
    if (range_secs >= 24 * 3600 && times[0] > cutoff + 3600) {
        g_free(times);
        g_free(ca);
        g_free(cmin);
        g_free(cmax);
        g_free(ma);
        g_free(mmin);
        g_free(mmax);
        return 0;
    }

    gint bucket = (got + HIST_POINTS - 1) / HIST_POINTS;

    if (bucket < 1)
        bucket = 1;

    guint n = 0;

    for (gint i = 0; i < got && n < out->cap; i += bucket) {
        double sc = 0.0, sm = 0.0;
        double lc = INFINITY, hc = -INFINITY;
        double lm = INFINITY, hm = -INFINITY;
        gint   cnt = 0;
        gint64 ts_last = 0;

        for (gint j = i; j < got && j < i + bucket; j++) {
            sc += ca[j];
            sm += ma[j];
            if (cmin[j] < lc)
                lc = cmin[j];
            if (cmax[j] > hc)
                hc = cmax[j];
            if (mmin[j] < lm)
                lm = mmin[j];
            if (mmax[j] > hm)
                hm = mmax[j];
            ts_last = times[j];
            cnt++;
        }

        if (cnt == 0)
            continue;

        out->cpu_avg[n] = sc / cnt;
        out->cpu_min[n] = lc;
        out->cpu_max[n] = hc;
        out->mem_avg[n] = sm / cnt;
        out->mem_min[n] = lm;
        out->mem_max[n] = hm;
        out->time_us[n] =
            (ts_last > 0 && ts_last < 10000000000LL)
                ? (uint64_t)ts_last * 1000000ULL
                : 0;
        n++;
    }

    g_free(times);
    g_free(ca);
    g_free(cmin);
    g_free(cmax);
    g_free(ma);
    g_free(mmin);
    g_free(mmax);

    *out_interval_secs = (gint64)bucket * 60;
    *avg_cpu = sumc / got;
    *avg_mem = summ / got;
    *max_cpu = peak;

    return n;
}

/* 读取 history.tsv，按范围降采样；返回点数（0 表示没有数据）。
 * 每个桶输出平均/最小/最大和桶末时间戳。 */
static guint load_history(gint64 range_secs, const HistoryOut *out,
                          gint64 *out_interval_secs, gdouble *avg_cpu,
                          gdouble *max_cpu, gdouble *avg_mem)
{
    if (out == NULL || out->cap == 0)
        return 0;

    gchar *content = NULL;
    gsize  len     = 0;

    if (!g_file_get_contents(history_path(), &content, &len, NULL) ||
        len == 0) {
        g_free(content);
        return 0;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    gint   total  = (gint)g_strv_length(lines);
    gint64 cutoff = (gint64)time(NULL) - range_secs;

    /* 先统计范围内的行数 */
    gint64 n_in = 0;

    for (gint i = 0; i < total; i++) {
        long long ts;
        double    v;

        if (sscanf(lines[i], "%lld\t%lf", &ts, &v) == 2 && ts >= cutoff)
            n_in++;
    }

    if (n_in <= 0) {
        g_strfreev(lines);
        return 0;
    }

    /* 收集范围内全部原始采样。旧版先按 step 抽掉一批点再做桶平均，
     * 两轮抽样会漏掉尖峰，且 24h 视图的平均值取决于抽样相位。
     * 这里只做一次按时间桶聚合，平均/极值都基于原始点计算。 */
    gint64 *times = g_new0(gint64, (gsize)n_in);
    double *cpus  = g_new0(double, (gsize)n_in);
    double *mems  = g_new0(double, (gsize)n_in);
    gint    got   = 0;
    double  sumc_raw = 0.0;
    double  summ_raw = 0.0;
    double  peak = 0.0;

    for (gint i = 0; i < total && got < (gint)n_in; i++) {
        long long ts;
        double    cpu, mem;

        if (sscanf(lines[i], "%lld\t%lf\t%lf", &ts, &cpu, &mem) != 3)
            continue;
        if (ts < cutoff)
            continue;
        if (!isfinite(cpu) || !isfinite(mem))
            continue;

        times[got] = (gint64)ts;
        cpus[got]  = cpu;
        mems[got]  = mem;
        sumc_raw += cpu;
        summ_raw += mem;
        if (cpu > peak)
            peak = cpu;
        got++;
    }

    g_strfreev(lines);

    if (got <= 0) {
        g_free(times);
        g_free(cpus);
        g_free(mems);
        return 0;
    }

    /* 单次桶聚合降到 HIST_POINTS 以内；时间戳取桶内最后一个采样。 */
    gint bucket = (got + HIST_POINTS - 1) / HIST_POINTS;

    if (bucket < 1)
        bucket = 1;

    guint n = 0;

    for (gint i = 0; i < got && n < out->cap; i += bucket) {
        gint   cnt = 0;
        double sc = 0.0, sm = 0.0;
        double cmin = 0.0, cmax = 0.0;
        double mmin = 0.0, mmax = 0.0;
        gint64 ts_last = 0;

        for (gint j = i; j < got && j < i + bucket; j++) {
            if (cnt == 0) {
                cmin = cmax = cpus[j];
                mmin = mmax = mems[j];
            } else {
                if (cpus[j] < cmin)
                    cmin = cpus[j];
                if (cpus[j] > cmax)
                    cmax = cpus[j];
                if (mems[j] < mmin)
                    mmin = mems[j];
                if (mems[j] > mmax)
                    mmax = mems[j];
            }

            sc += cpus[j];
            sm += mems[j];
            ts_last = times[j];
            cnt++;
        }

        if (cnt == 0)
            continue;

        out->cpu_avg[n] = sc / cnt;
        out->cpu_min[n] = cmin;
        out->cpu_max[n] = cmax;
        out->mem_avg[n] = sm / cnt;
        out->mem_min[n] = mmin;
        out->mem_max[n] = mmax;
        /* 合理范围之外的 TSV 时间戳（损坏/恶意文件）视为无效，
         * 避免乘以 1e6 溢出，绘图端会退回等间距。 */
        out->time_us[n] = (ts_last > 0 && ts_last < 10000000000LL)
                              ? (uint64_t)ts_last * 1000000ULL
                              : 0;
        n++;
    }

    g_free(times);
    g_free(cpus);
    g_free(mems);

    *out_interval_secs = (gint64)bucket * 10;   /* 守护进程每 10 秒写一行 */
    *avg_cpu = (got > 0) ? sumc_raw / got : 0.0;
    *avg_mem = (got > 0) ? summ_raw / got : 0.0;
    *max_cpu = peak;

    return n;
}

void history_refresh(HistoryPage *page)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->range_dd));
    gint64 range_secs;
    const char *range_name;

    switch (sel) {
    case 0:
        range_secs = 5 * 60;
        range_name = "最近 5 分钟";
        break;
    case 1:
        range_secs = 15 * 60;
        range_name = "最近 15 分钟";
        break;
    case 3:
        range_secs = 24 * 3600;
        range_name = "最近 24 小时";
        break;
    default:
        range_secs = 3600;
        range_name = "最近 1 小时";
        break;
    }

    HistoryOut out = {
        .cpu_avg = g_cpu,
        .cpu_min = g_cpu_min,
        .cpu_max = g_cpu_max,
        .mem_avg = g_mem,
        .mem_min = g_mem_min,
        .mem_max = g_mem_max,
        .time_us = g_time_us,
        .cap     = HIST_POINTS,
    };

    gint64  interval = 10;
    gdouble avg_cpu = 0.0, max_cpu = 0.0, avg_mem = 0.0;

    guint n = 0;
    gboolean pyramid = FALSE;

    if (range_secs >= 24 * 3600 &&
        g_file_test(history_1m_path(), G_FILE_TEST_EXISTS)) {
        n = load_history_1m(range_secs, &out, &interval, &avg_cpu, &max_cpu,
                            &avg_mem);
        pyramid = n > 0;
    }

    if (n == 0)
        n = load_history(range_secs, &out, &interval, &avg_cpu, &max_cpu,
                         &avg_mem);

    page->cpu_cd.history     = g_cpu;
    page->cpu_cd.history_min = g_cpu_min;
    page->cpu_cd.history_max = g_cpu_max;
    page->cpu_cd.time_us     = g_time_us;
    page->cpu_cd.count       = n;
    page->cpu_cd.index       = n;
    page->cpu_cd.interval_us = (uint64_t)interval * 1000000ULL;

    page->mem_cd.history     = g_mem;
    page->mem_cd.history_min = g_mem_min;
    page->mem_cd.history_max = g_mem_max;
    page->mem_cd.time_us     = g_time_us;
    page->mem_cd.count       = n;
    page->mem_cd.index       = n;
    page->mem_cd.interval_us = (uint64_t)interval * 1000000ULL;
    page->n = n;
    page->interval_s = (guint)interval;

    uint64_t vs = 0, ve = 0;

    if (page->custom && n > 1) {
        uint64_t first = g_time_us[0];
        uint64_t last  = g_time_us[n - 1];

        vs = page->view_start_us;
        ve = page->view_end_us;

        if (vs < first)
            vs = first;
        if (ve == 0 || ve > last)
            ve = last;
        if (ve <= vs) {
            page->custom = FALSE;
            vs = ve = 0;
        }
    }

    page->cpu_cd.view_start_us = vs;
    page->cpu_cd.view_end_us   = ve;
    page->cpu_cd.fixed_range   = TRUE;
    page->mem_cd.view_start_us = vs;
    page->mem_cd.view_end_us   = ve;
    page->mem_cd.fixed_range   = TRUE;

    gtk_widget_queue_draw(page->cpu_area);
    gtk_widget_queue_draw(page->mem_area);

    if (n == 0) {
        gtk_label_set_text(GTK_LABEL(page->summary),
                           "没有历史数据（守护进程运行后每 10 秒记录一次）");
    } else if (page->custom) {
        gchar *text = g_strdup_printf(
            "框选窗口：%u 个采样点（双击图表恢复 %s 完整范围；"
            "CSV 导出当前框选范围）",
            n, range_name);
        gtk_label_set_text(GTK_LABEL(page->summary), text);
        g_free(text);
    } else {
        gchar *text = g_strdup_printf(
            "%s：%u 个采样点%s · CPU 平均 %.1f%% / 原始峰值 %.1f%% · "
            "内存平均 %.1f%% · 阴影为桶内最小–最大",
            range_name, n, pyramid ? "（1 分钟金字塔）" : "", avg_cpu,
            max_cpu, avg_mem);
        gtk_label_set_text(GTK_LABEL(page->summary), text);
        g_free(text);
    }
}
