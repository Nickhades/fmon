/*
 * history_view.c - 指标历史页面
 *
 * 读取守护进程每 10 秒写入的 history.tsv，按 1 小时 / 24 小时范围
 * 降采样到最多 FMON_HISTORY_POINTS 个点，用曲线展示 CPU 与内存。
 */

#include "history_view.h"

#include "gui_chart.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <string.h>
#include <time.h>

#define HIST_POINTS FMON_HISTORY_POINTS

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

static double g_cpu[HIST_POINTS];
static double g_mem[HIST_POINTS];

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

    if (g_file_test("/var/db/fmon/history.tsv", G_FILE_TEST_EXISTS)) {
        g_strlcpy(path, "/var/db/fmon/history.tsv", sizeof(path));
    } else {
        const char *home = g_get_home_dir();

        g_snprintf(path, sizeof(path), "%s/.local/state/fmon/history.tsv",
                   (home != NULL) ? home : "/tmp");
    }

    return path;
}

/* 读取 history.tsv，按范围降采样；返回点数（0 表示没有数据） */
static guint load_history(gint64 range_secs, double *out_cpu, double *out_mem,
                          gint64 *out_interval_secs, gdouble *avg_cpu,
                          gdouble *max_cpu, gdouble *avg_mem)
{
    gchar *content = NULL;
    gsize  len     = 0;

    if (!g_file_get_contents(history_path(), &content, &len, NULL) ||
        len == 0) {
        g_free(content);
        return 0;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    gint total = (gint)g_strv_length(lines);
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

    /* 收集到临时数组（最多 HIST_POINTS*8 个，超出则先粗采样） */
    gint     cap   = (n_in > HIST_POINTS * 8) ? HIST_POINTS * 8 : (gint)n_in;
    double  *cpus  = g_new0(double, (gsize)cap);
    double  *mems  = g_new0(double, (gsize)cap);
    gint     got   = 0;
    gint64   step  = (n_in > cap) ? (n_in / cap) : 1;
    gint64   seen  = 0;

    for (gint i = 0; i < total && got < cap; i++) {
        long long ts;
        double    cpu, mem;

        if (sscanf(lines[i], "%lld\t%lf\t%lf", &ts, &cpu, &mem) != 3)
            continue;
        if (ts < cutoff)
            continue;
        if ((seen++ % step) != 0)
            continue;

        cpus[got] = cpu;
        mems[got] = mem;
        got++;
    }

    g_strfreev(lines);

    if (got <= 0) {
        g_free(cpus);
        g_free(mems);
        return 0;
    }

    /* 桶平均降到 HIST_POINTS 以内 */
    gint bucket = (got + HIST_POINTS - 1) / HIST_POINTS;

    if (bucket < 1)
        bucket = 1;

    gint   n    = 0;
    double sumc = 0.0, summ = 0.0;
    double peak = 0.0;

    for (gint i = 0; i < got; i += bucket) {
        gint    cnt = 0;
        double  sc = 0.0, sm = 0.0;

        for (gint j = i; j < got && j < i + bucket; j++) {
            sc += cpus[j];
            sm += mems[j];
            if (cpus[j] > peak)
                peak = cpus[j];
            cnt++;
        }

        if (cnt == 0)
            continue;

        out_cpu[n] = sc / cnt;
        out_mem[n] = sm / cnt;
        sumc += out_cpu[n];
        summ += out_mem[n];
        n++;
    }

    g_free(cpus);
    g_free(mems);

    *out_interval_secs = (gint64)bucket * step * 10;   /* 采样间隔是 10 秒 */
    *avg_cpu = (n > 0) ? sumc / n : 0.0;
    *avg_mem = (n > 0) ? summ / n : 0.0;
    *max_cpu = peak;

    return (guint)n;
}

static void refresh_history(HistoryPage *page)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->range_dd));
    gint64 range_secs = (sel == 1) ? 24 * 3600 : 3600;

    gint64  interval = 10;
    gdouble avg_cpu = 0.0, max_cpu = 0.0, avg_mem = 0.0;

    guint n = load_history(range_secs, g_cpu, g_mem, &interval, &avg_cpu,
                           &max_cpu, &avg_mem);

    page->cpu_cd.history    = g_cpu;
    page->cpu_cd.count      = n;
    page->cpu_cd.index      = n;
    page->cpu_cd.interval_us = (uint64_t)interval * 1000000ULL;

    page->mem_cd.history    = g_mem;
    page->mem_cd.count      = n;
    page->mem_cd.index      = n;
    page->mem_cd.interval_us = (uint64_t)interval * 1000000ULL;

    gtk_widget_queue_draw(page->cpu_area);
    gtk_widget_queue_draw(page->mem_area);

    if (n == 0) {
        gtk_label_set_text(GTK_LABEL(page->summary),
                           "没有历史数据（守护进程运行后每 10 秒记录一次）");
    } else {
        gchar *text = g_strdup_printf(
            "%s：%u 个采样点 · CPU 平均 %.1f%% / 峰值 %.1f%% · 内存平均 %.1f%%",
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
                   make_card("CPU 历史", &page->cpu_area, &page->cpu_cd,
                             0.30, 0.85, 0.45));
    gtk_box_append(GTK_BOX(cards),
                   make_card("内存历史", &page->mem_area, &page->mem_cd,
                             0.35, 0.65, 0.95));
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
