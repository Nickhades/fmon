/*
 * gui_cpu_detail.c - CPU 时间分布 / 硬件型号 / 调度状态
 */

#include "gui_cpu_detail.h"

#include "cpu_stats.h"
#include "gui_state.h"
#include "ui_metrics.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

/* 时间分布：5 列等宽 */
static GtkWidget *s_bar = NULL;
static GtkWidget *s_time_val[CPU_TIME_COUNT];
static GtkWidget *s_key[8];

/* 状态网格：8 个值（4 行 × 2 组） */
enum {
    V_FREQ = 0,
    V_FREQ_MAX,
    V_CS,
    V_IN,
    V_LOAD,
    V_CPUS,
    V_SOCKETS,
    V_VIRT,
    V_COUNT
};

static GtkWidget *s_val[V_COUNT];
static GtkWidget *s_note = NULL;
static GtkWidget *s_virt_note = NULL;
static GtkWidget *s_hw_line = NULL;

static const char *const TIME_NAMES[CPU_TIME_COUNT] = {
    "用户", "nice", "系统", "中断", "空闲",
};

static const char *const TIME_DESC[CPU_TIME_COUNT] = {
    "应用进程", "低优先级进程", "内核与系统调用", "硬件/软件中断", "CPU 未使用",
};

static const char *const TIME_TIP[CPU_TIME_COUNT] = {
    "用户态 CPU 时间：普通应用程序（浏览器、编辑器、编译器）占用",
    "低优先级（nice > 0）用户进程占用",
    "内核态时间：系统调用、驱动、内核线程",
    "处理硬件/软件中断的时间",
    "没有任务可运行的时间；越低说明 CPU 越忙",
};

static const char *const TIME_HEX[CPU_TIME_COUNT] = {
    "#428CF2", "#59BD75", "#9E7ADB", "#F2B340", "#9AA0A6",
};

static const double TIME_COLOR[CPU_TIME_COUNT][3] = {
    { 0.26, 0.55, 0.95 },
    { 0.35, 0.74, 0.46 },
    { 0.62, 0.48, 0.86 },
    { 0.95, 0.70, 0.25 },
    { 0.80, 0.82, 0.85 },
};

static void bar_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                     gpointer data)
{
    (void)area;
    (void)data;

    const CpuStats *cs = cpu_stats_get();
    double x = 0.0;

    for (int t = 0; t < CPU_TIME_COUNT; t++) {
        double frac = (cs != NULL) ? cs->pct[t] / 100.0 : 0.0;
        double w    = frac * (double)width;

        cairo_set_source_rgb(cr, TIME_COLOR[t][0], TIME_COLOR[t][1],
                             TIME_COLOR[t][2]);
        cairo_rectangle(cr, x, 0.0, w, (double)height);
        cairo_fill(cr);

        x += w;
    }

    if (x < (double)width) {
        cairo_set_source_rgb(cr, TIME_COLOR[CPU_TIME_IDLE][0],
                             TIME_COLOR[CPU_TIME_IDLE][1],
                             TIME_COLOR[CPU_TIME_IDLE][2]);
        cairo_rectangle(cr, x, 0.0, (double)width - x, (double)height);
        cairo_fill(cr);
    }
}

GtkWidget *gui_cpu_hw_header(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_halign(box, GTK_ALIGN_END);
    gtk_widget_set_valign(box, GTK_ALIGN_START);

    s_hw_line = gtk_label_new("");
    gtk_widget_add_css_class(s_hw_line, "hw-model");
    gtk_label_set_xalign(GTK_LABEL(s_hw_line), 1.0f);
    gtk_widget_set_halign(s_hw_line, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(s_hw_line), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(s_hw_line, 520, -1);

    gtk_box_append(GTK_BOX(box), s_hw_line);
    return box;
}

void gui_cpu_hw_update(void)
{
    if (s_hw_line == NULL)
        return;

    const char *model = gui_state_snapshot()->cpu_model;

    gtk_label_set_text(GTK_LABEL(s_hw_line),
                       (model[0] != '\0') ? model : "");
}

static GtkWidget *label_x(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_widget_set_halign(l, (xalign >= 1.0f) ? GTK_ALIGN_END : GTK_ALIGN_START);
    return l;
}

static GtkWidget *section_title(const char *text)
{
    GtkWidget *l = label_x(text, "detail-title", 0.0f);

    gtk_widget_set_margin_top(l, FMON_SPACE_3);
    return l;
}

GtkWidget *gui_cpu_detail_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_1);

    gtk_box_append(GTK_BOX(box), section_title(_("CPU 时间分布")));

    s_bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_bar, -1, 18);
    gtk_widget_set_hexpand(s_bar, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_bar), bar_draw, NULL,
                                   NULL);
    gtk_box_append(GTK_BOX(box), s_bar);

    /* 图例：5 列等宽，数值右对齐，避免每行长短不一 */
    GtkWidget *legend = gtk_grid_new();

    gtk_grid_set_column_spacing(GTK_GRID(legend), FMON_SPACE_5);
    gtk_widget_set_margin_top(legend, FMON_SPACE_1);
    gtk_widget_set_halign(legend, GTK_ALIGN_START);

    for (int t = 0; t < CPU_TIME_COUNT; t++) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        s_time_val[t] = label_x("—", "group-desc", 0.0f);
        /* 定宽：百分比位数变化时 5 列不互相推挤 */
        gtk_widget_set_size_request(s_time_val[t], 130, -1);
        gtk_widget_set_tooltip_text(s_time_val[t], _(TIME_TIP[t]));

        GtkWidget *sub = label_x(_(TIME_DESC[t]), "legend-note", 0.0f);

        gtk_box_append(GTK_BOX(col), s_time_val[t]);
        gtk_box_append(GTK_BOX(col), sub);
        gtk_grid_attach(GTK_GRID(legend), col, t, 0, 1, 1);
    }

    gtk_box_append(GTK_BOX(box), legend);

    return box;
}

/* 状态与调度：独立成块，放进仪表卡片右侧空白处 */
GtkWidget *gui_cpu_status_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    /* 状态：4 列（键、值、键、值），行距统一 */
    gtk_box_append(GTK_BOX(box), section_title(_("状态与调度")));

    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(grid), FMON_SPACE_1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), FMON_SPACE_2);

    static const char *const KEYS[V_COUNT] = {
        "当前频率", "最高档", "上下文切换", "中断",
        "平均负载 1/5/15", "逻辑处理器", "插槽", "虚拟化",
    };

    for (int i = 0; i < V_COUNT; i++) {
        int row = i / 2;
        int col = (i % 2) * 2;

        GtkWidget *k = label_x(KEYS[i], "detail-key", 0.0f);

        s_key[i] = k;
        gtk_widget_set_halign(k, GTK_ALIGN_START);
        gtk_widget_set_size_request(k, (i == V_LOAD) ? 124 : 96, -1);
        gtk_grid_attach(GTK_GRID(grid), k, col, row, 1, 1);

        s_val[i] = label_x("—", "detail-value", 0.0f);
        gtk_widget_set_size_request(s_val[i], (i == V_LOAD) ? 168 : 118, -1);
        gtk_widget_set_hexpand(s_val[i], FALSE);
        gtk_grid_attach(GTK_GRID(grid), s_val[i], col + 1, row, 1, 1);

        if (col == 2) {
            GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

            gtk_widget_set_size_request(gap, 18, -1);
            gtk_grid_attach(GTK_GRID(grid), gap, 1, row, 1, 1);
        }
    }

    gtk_widget_set_visible(s_key[V_LOAD], FALSE);
    gtk_widget_set_visible(s_val[V_LOAD], FALSE);

    gtk_box_append(GTK_BOX(box), grid);

    s_note = label_x("", "group-desc", 0.0f);
    gtk_widget_set_margin_top(s_note, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_note);

    s_virt_note = label_x("", "group-desc", 0.0f);
    gtk_label_set_wrap(GTK_LABEL(s_virt_note), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(s_virt_note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(s_virt_note), 96);
    gtk_label_set_xalign(GTK_LABEL(s_virt_note), 0.0f);
    gtk_widget_set_margin_top(s_virt_note, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_virt_note);

    return box;
}

static void fmt_rate(char *buf, size_t len, double per_s)
{
    if (per_s >= 1e6)
        g_snprintf(buf, len, "%.1fM/s", per_s / 1e6);
    else if (per_s >= 1e3)
        g_snprintf(buf, len, "%.1fk/s", per_s / 1e3);
    else
        g_snprintf(buf, len, "%.0f/s", per_s);
}

void gui_cpu_detail_update(void)
{
    const CpuStats    *cs   = cpu_stats_get();
    const FmonShmData *snap = gui_state_snapshot();

    if (cs == NULL)
        return;

    /* CPU 页不可见时跳过：时间分布标签不在概览页 */
    if (s_time_val[0] == NULL || !gtk_widget_get_mapped(s_time_val[0]))
        return;

    for (int t = 0; t < CPU_TIME_COUNT; t++) {
        gchar *txt = g_strdup_printf(
            "<span foreground=\"%s\">■</span> %s <b>%.1f%%</b>",
            TIME_HEX[t], _(TIME_NAMES[t]), cs->pct[t]);

        gtk_label_set_markup(GTK_LABEL(s_time_val[t]), txt);
        g_free(txt);
    }

    int have = 0, first = -1;

    for (int i = 0; i < cs->ncpu && i < FMON_MAX_CPUS; i++) {
        if (cs->freq_mhz[i] > 0.0) {
            have++;
            if (first < 0)
                first = i;
        }
    }

    char b[64];

    if (have > 0)
        g_snprintf(b, sizeof(b), "%.2f GHz",
                   cs->freq_mhz[have == 1 ? first : 0] / 1000.0);
    else
        g_snprintf(b, sizeof(b), "—");
    gtk_label_set_text(GTK_LABEL(s_val[V_FREQ]), b);

    if (cs->freq_max > 0.0)
        g_snprintf(b, sizeof(b), "%.2f GHz", cs->freq_max / 1000.0);
    else
        g_snprintf(b, sizeof(b), "—");
    gtk_label_set_text(GTK_LABEL(s_val[V_FREQ_MAX]), b);

    fmt_rate(b, sizeof(b), cs->cs_per_s);
    gtk_label_set_text(GTK_LABEL(s_val[V_CS]), b);

    fmt_rate(b, sizeof(b), cs->in_per_s);
    gtk_label_set_text(GTK_LABEL(s_val[V_IN]), b);

    /* 平均负载：快照里的 1/5/15 分钟；缺失时退回 1 分钟值 */
    double l1 = (snap->load_avg[0] > 0.0) ? snap->load_avg[0] : cs->load1;

    g_snprintf(b, sizeof(b), "%.2f / %.2f / %.2f", l1, snap->load_avg[1],
               snap->load_avg[2]);
    gtk_label_set_text(GTK_LABEL(s_val[V_LOAD]), b);

    g_snprintf(b, sizeof(b), "%u", (unsigned)snap->cpu_count);
    gtk_label_set_text(GTK_LABEL(s_val[V_CPUS]), b);

    g_snprintf(b, sizeof(b), "%d", cs->sockets);
    gtk_label_set_text(GTK_LABEL(s_val[V_SOCKETS]), b);

    gtk_label_set_text(GTK_LABEL(s_val[V_VIRT]), cs->virt.state);
    gtk_widget_set_tooltip_text(s_val[V_VIRT], cs->virt.detail);

    if (s_virt_note != NULL) {
        gtk_label_set_text(GTK_LABEL(s_virt_note), cs->virt.detail);
    }

    /* 基准频率与平台调频说明放在网格下方一行，避免再占一行键值 */
    GString *note = g_string_new(NULL);

    if (cs->base_mhz > 0.0)
        g_string_append_printf(note, _("基准频率 %.2f GHz"),
                               cs->base_mhz / 1000.0);

    if (have == 1) {
        if (note->len > 0)
            g_string_append(note, " · ");
        g_string_append(note, _("该平台按封装调频，仅 CPU0 暴露频率"));
    }

    gtk_label_set_text(GTK_LABEL(s_note), note->str);
    g_string_free(note, TRUE);

    if (s_bar != NULL)
        gtk_widget_queue_draw(s_bar);
}
