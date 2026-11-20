/*
 * history_view.c - 指标历史页面
 *
 * 读取守护进程每 10 秒写入的 history.tsv，按 1 小时 / 24 小时范围
 * 降采样到最多 FMON_HISTORY_POINTS 个点，用曲线展示 CPU 与内存。
 *
 * 24 小时视图每个时间桶保留真实 min/max：曲线本身仍是桶平均，
 * 背景色带显示该桶内 10 秒原始采样的极值，避免平均把尖峰藏起来。
 */

#include "history_view.h"

#include "gui_chart.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define HIST_POINTS FMON_HISTORY_POINTS

typedef struct {
    double  *cpu_avg;
    double  *cpu_min;
    double  *cpu_max;
    double  *mem_avg;
    double  *mem_min;
    double  *mem_max;
    uint64_t *time_us;
    guint    cap;
} HistoryOut;

typedef struct {
    GtkWidget *root;
    GtkWidget *range_dd;
    GtkWidget *summary;
    GtkWidget *cpu_area;
    GtkWidget *mem_area;
    CurveData  cpu_cd;
    CurveData  mem_cd;
    guint      timer;
} HistoryPage;

static double   g_cpu[HIST_POINTS];
static double   g_cpu_min[HIST_POINTS];
static double   g_cpu_max[HIST_POINTS];
static double   g_mem[HIST_POINTS];
static double   g_mem_min[HIST_POINTS];
static double   g_mem_max[HIST_POINTS];
static uint64_t g_time_us[HIST_POINTS];

static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);

    if (id != 0)
        g_source_remove(id);
}

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

static void refresh_history(HistoryPage *page)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->range_dd));
    gint64 range_secs = (sel == 1) ? 24 * 3600 : 3600;

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

    guint n = load_history(range_secs, &out, &interval, &avg_cpu, &max_cpu,
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

    gtk_widget_queue_draw(page->cpu_area);
    gtk_widget_queue_draw(page->mem_area);

    if (n == 0) {
        gtk_label_set_text(GTK_LABEL(page->summary),
                           "没有历史数据（守护进程运行后每 10 秒记录一次）");
    } else {
        gchar *text = g_strdup_printf(
            "%s：%u 个采样点 · CPU 平均 %.1f%% / 原始峰值 %.1f%% · "
            "内存平均 %.1f%% · 阴影为桶内最小–最大",
            (sel == 1) ? "最近 24 小时" : "最近 1 小时", n, avg_cpu, max_cpu,
            avg_mem);
        gtk_label_set_text(GTK_LABEL(page->summary), text);
        g_free(text);
    }
}

static void on_range_changed(GtkDropDown *dd, GParamSpec *pspec,
                             gpointer data)
{
    (void)dd;
    (void)pspec;

    refresh_history(data);
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    refresh_history(data);
}

static gboolean on_tick(gpointer data)
{
    HistoryPage *page = data;

    if (page->root == NULL || !gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    refresh_history(page);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *make_card(const char *title, GtkWidget **area_out,
                            CurveData *cd, gdouble r, gdouble g, gdouble b)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card");

    GtkWidget *label = gtk_label_new(title);
    gtk_widget_add_css_class(label, "title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), label);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 170);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_box_append(GTK_BOX(card), area);

    cd->max_val    = 100.0;
    cd->r          = r;
    cd->g          = g;
    cd->b          = b;
    cd->unit       = "%";
    cd->empty_hint = "没有历史数据";

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), gui_chart_draw, cd,
                                   NULL);

    *area_out = area;
    return card;
}

GtkWidget *gui_history_view_new(void)
{
    HistoryPage *page = g_new0(HistoryPage, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_add_css_class(vbox, "view-card");
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_refresh = gtk_button_new_with_label("刷新");
    gtk_widget_add_css_class(btn_refresh, "suggested-action");

    const char *ranges[] = { "最近 1 小时", "最近 24 小时", NULL };
    page->range_dd = gtk_drop_down_new_from_strings(ranges);

    gtk_box_append(GTK_BOX(toolbar), btn_refresh);
    gtk_box_append(GTK_BOX(toolbar), page->range_dd);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    page->summary = gtk_label_new("加载中…");
    gtk_widget_add_css_class(page->summary, "small-text");
    gtk_widget_set_halign(page->summary, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), page->summary);

    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_vexpand(cards, TRUE);

    gtk_box_append(GTK_BOX(cards),
                   make_card("CPU 历史 · 平均 + 极值带", &page->cpu_area,
                             &page->cpu_cd, 0.30, 0.85, 0.45));
    gtk_box_append(GTK_BOX(cards),
                   make_card("内存历史 · 平均 + 极值带", &page->mem_area,
                             &page->mem_cd, 0.35, 0.65, 0.95));
    gtk_box_append(GTK_BOX(vbox), cards);

    GtkWidget *hint = gtk_label_new(
        "历史数据由守护进程每 10 秒写入 /var/db/fmon/history.tsv（普通用户调试时为 ~/.local/state/fmon/history.tsv）。");
    gtk_widget_add_css_class(hint, "small-text");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), hint);

    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     page);
    g_signal_connect(page->range_dd, "notify::selected",
                     G_CALLBACK(on_range_changed), page);

    refresh_history(page);

    page->timer = g_timeout_add_seconds(30, on_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "history-timer",
                           GUINT_TO_POINTER(page->timer), remove_source);
    g_object_set_data_full(G_OBJECT(vbox), "history-page", page, g_free);

    return vbox;
}
