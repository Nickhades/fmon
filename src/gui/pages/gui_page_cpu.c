/*
 * gui_page_cpu.c - CPU 页扩展：时间分布堆叠条、频率、调度状态（对齐网格）
 *
 * 每核占用由 gui_coreview 的方块展示；这里用统一的键值网格补齐
 * 时间构成、频率与调度压力，避免多行文字宽度不一造成的参差。
 */

#include "gui_page_cpu.h"

#include "chart_geom.h"
#include "combo_chart.h"
#include "cpu_health.h"
#include "cpu_stats.h"
#include "gui_pages.h"
#include "gui_state.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>
#include <stdlib.h>

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

    gtk_widget_set_margin_top(l, 10);
    return l;
}

GtkWidget *gui_cpu_detail_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    gtk_widget_set_margin_top(box, 2);

    gtk_box_append(GTK_BOX(box), section_title(_("CPU 时间分布")));

    s_bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_bar, -1, 18);
    gtk_widget_set_hexpand(s_bar, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_bar), bar_draw, NULL,
                                   NULL);
    gtk_box_append(GTK_BOX(box), s_bar);

    /* 图例：5 列等宽，数值右对齐，避免每行长短不一 */
    GtkWidget *legend = gtk_grid_new();

    gtk_grid_set_column_spacing(GTK_GRID(legend), 18);
    gtk_widget_set_margin_top(legend, 2);
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
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    /* 状态：4 列（键、值、键、值），行距统一 */
    gtk_box_append(GTK_BOX(box), section_title(_("状态与调度")));

    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

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
    gtk_widget_set_margin_top(s_note, 2);
    gtk_box_append(GTK_BOX(box), s_note);

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

/* ===========================================================================
 * 半圆仪表卡片
 * ========================================================================= */

static GtkWidget *s_gauge_area;
static GtkWidget *s_health;
static double     s_g_cur;
static double     s_g_avg;
static double     s_g_load[3];
static unsigned   s_g_ncpu;

static void gauge_font(cairo_t *cr, double size)
{
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, size);
}

static void gauge_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer data)
{
    (void)data;

    double r = MIN(w * 0.16, h * 0.80);

    if (r < 24.0)
        r = 24.0;

    double cx = w * 0.18;
    double cy = h - 14.0;
    double frac = s_g_cur / 100.0;

    if (frac < 0.0)
        frac = 0.0;
    if (frac > 1.0)
        frac = 1.0;

    cairo_set_line_width(cr, MAX(6.0, r * 0.10));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_set_source_rgba(cr, 0.20, 0.78, 0.45, 0.14);
    cairo_arc(cr, cx, cy, r, G_PI, 2.0 * G_PI);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.20, 0.78, 0.45, 0.95);
    cairo_arc(cr, cx, cy, r, G_PI, G_PI + G_PI * frac);
    cairo_stroke(cr);

    char txt[96];
    cairo_text_extents_t ext;

    gauge_font(cr, r * 0.30);
    g_snprintf(txt, sizeof(txt), "%.1f%%", s_g_cur);
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgba(cr, 0.20, 0.78, 0.45, 0.95);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, cy - r * 0.34);
    cairo_show_text(cr, txt);

    gauge_font(cr, r * 0.13);
    g_snprintf(txt, sizeof(txt), "近 5 分钟平均 %.1f%%", s_g_avg);
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgba(cr, 0.20, 0.78, 0.45, 0.70);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, cy - r * 0.05);
    cairo_show_text(cr, txt);

    /* 右侧：1 / 5 / 15 分钟平均负载 */
    GdkRGBA fg;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);

    double tx = w * 0.42;

    gauge_font(cr, 13.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.60);
    cairo_move_to(cr, tx, cy - r * 0.52);
    if (s_g_ncpu > 0) {
        char head[192];

        g_snprintf(head, sizeof(head),
                   "系统平均负载（可运行任务数 · 每线程 %.2f / %.2f / %.2f）",
                   s_g_load[0] / (double)s_g_ncpu,
                   s_g_load[1] / (double)s_g_ncpu,
                   s_g_load[2] / (double)s_g_ncpu);
        cairo_show_text(cr, head);
    } else {
        cairo_show_text(cr, "系统平均负载（可运行任务数平均值）");
    }

    gauge_font(cr, 24.0);
    g_snprintf(txt, sizeof(txt), "%.2f   /   %.2f   /   %.2f",
               s_g_load[0], s_g_load[1], s_g_load[2]);

    double lq = (s_g_ncpu > 0) ? s_g_load[0] / (double)s_g_ncpu : 0.0;

    if (lq > 2.0)
        cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, 0.95);
    else if (lq > 1.0)
        cairo_set_source_rgba(cr, 0.90, 0.65, 0.04, 0.95);
    else
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.95);

    cairo_move_to(cr, tx, cy - r * 0.20);
    cairo_show_text(cr, txt);

    gauge_font(cr, 12.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.55);
    cairo_move_to(cr, tx, cy - r * 0.02);
    cairo_show_text(cr,
                    "1 分钟 / 5 分钟 / 15 分钟（单位：任务数；每线程 <1 不排队）");
}

static GtkWidget *s_gauge_header_right;
static GtkWidget *s_gauge_body;

static double hist_at(const CurveData *cd, uint32_t i);
static double avg_window_value(const CurveData *cd, uint32_t win);

GtkWidget *gui_cpu_gauge_header_right(void)
{
    return s_gauge_header_right;
}

GtkWidget *gui_cpu_gauge_body(void)
{
    return s_gauge_body;
}

GtkWidget *gui_cpu_gauge_card(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_add_css_class(card, "page-card");

    GtkWidget *t = gtk_label_new(_("CPU 使用率"));

    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_widget_set_halign(t, GTK_ALIGN_START);

    GtkWidget *s = gtk_label_new(_("当前值 · 历史平均 · 1/5/15 分钟负载"));

    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_widget_set_halign(s, GTK_ALIGN_START);

    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_hexpand(title_box, TRUE);
    gtk_box_append(GTK_BOX(title_box), t);
    gtk_box_append(GTK_BOX(title_box), s);

    s_gauge_header_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(s_gauge_header_right, GTK_ALIGN_END);
    gtk_widget_set_valign(s_gauge_header_right, GTK_ALIGN_START);

    s_health = gtk_label_new(_("设备正常"));
    gtk_widget_add_css_class(s_health, "health-ok");
    gtk_label_set_xalign(GTK_LABEL(s_health), 1.0f);
    gtk_widget_set_halign(s_health, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(s_gauge_header_right), s_health);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

    gtk_box_append(GTK_BOX(header), title_box);
    gtk_box_append(GTK_BOX(header), s_gauge_header_right);
    gtk_box_append(GTK_BOX(card), header);

    s_gauge_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_gauge_area, -1, 180);
    gtk_widget_set_hexpand(s_gauge_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_gauge_area), gauge_draw,
                                   NULL, NULL);
    gtk_widget_set_tooltip_text(
        s_gauge_area,
        "半圆：当前 CPU 使用率；内侧为最近 5 分钟的平均使用率，\n"
        "不足 5 分钟时按已有数据计算。\n"
        "右侧：系统平均负载 = 过去 1/5/15 分钟内平均可运行（运行中或\n"
        "等待 CPU）的任务数；除以线程数得到每线程负载：<1 不排队，\n"
        ">1 有排队，>2 明显拥堵。");
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    gtk_widget_set_hexpand(body, TRUE);
    gtk_box_append(GTK_BOX(body), s_gauge_area);
    s_gauge_body = body;
    gtk_box_append(GTK_BOX(card), body);

    return card;
}

void gui_cpu_gauge_update(void)
{
    if (s_gauge_area == NULL || !gtk_widget_get_mapped(s_gauge_area))
        return;

    const FmonShmData *snap = gui_state_snapshot();
    double cur = snap->cpu_usage_pct;
    double avg = 0.0;

    if (snap->history_count > 0) {
        guint last = (snap->history_index + FMON_HISTORY_POINTS - 1) %
                     FMON_HISTORY_POINTS;
        double sum = 0.0;

        /* 固定 5 分钟窗口；不足 5 分钟按已有数据 */
        guint win = (snap->history_count < 300) ? snap->history_count : 300;

        for (guint i = 0; i < win; i++) {
            guint idx = (last + FMON_HISTORY_POINTS - i) %
                        FMON_HISTORY_POINTS;

            sum += snap->cpu_history[idx];
        }

        cur = snap->cpu_history[last];
        avg = (win > 0) ? sum / (double)win : 0.0;
    }

    /* 设备健康判定：1 分钟平均使用率 + 1 分钟负载 + 当前温度 */
    const CurveData *use_cd  = gui_metric_curve(0);
    const CurveData *temp_cd = gui_metric_curve(12);
    double           u1      = (use_cd != NULL)
                                   ? avg_window_value(use_cd, 60)
                                   : cur;
    double           tcur    = (temp_cd != NULL && temp_cd->count > 0)
                                   ? hist_at(temp_cd, temp_cd->count - 1)
                                   : 0.0;
    char             hmsg[192];
    CpuHealth        hh = cpu_health_eval(u1, snap->load_avg[0],
                                          snap->cpu_count, tcur, hmsg,
                                          sizeof(hmsg));

    if (s_health != NULL &&
        g_strcmp0(gtk_label_get_text(GTK_LABEL(s_health)), hmsg) != 0) {
        gtk_label_set_text(GTK_LABEL(s_health), hmsg);
        gtk_widget_remove_css_class(s_health, "health-ok");
        gtk_widget_remove_css_class(s_health, "health-warn");
        gtk_widget_remove_css_class(s_health, "health-bad");
        gtk_widget_add_css_class(s_health,
                                 (hh == CPU_HEALTH_BAD)   ? "health-bad"
                                 : (hh == CPU_HEALTH_WARN) ? "health-warn"
                                                           : "health-ok");
    }

    if (fabs(cur - s_g_cur) < 0.001 && fabs(avg - s_g_avg) < 0.001 &&
        fabs(snap->load_avg[0] - s_g_load[0]) < 0.001 &&
        fabs(snap->load_avg[1] - s_g_load[1]) < 0.001 &&
        fabs(snap->load_avg[2] - s_g_load[2]) < 0.001)
        return;

    s_g_cur = cur;
    s_g_avg = avg;
    s_g_load[0] = snap->load_avg[0];
    s_g_load[1] = snap->load_avg[1];
    s_g_load[2] = snap->load_avg[2];
    s_g_ncpu   = snap->cpu_count;

    gtk_widget_queue_draw(s_gauge_area);
}

/* ===========================================================================
 * 负载 / 温度双折线图 + CPU 温度计
 * ========================================================================= */

static GtkWidget *s_thermal_area;

static double hist_at(const CurveData *cd, uint32_t i)
{
    double v;

    if (!fmon_chart_history_value(cd->history, FMON_HISTORY_POINTS,
                                  cd->count, cd->index, i, &v))
        return NAN;

    return v;
}

static void draw_axis(cairo_t *cr, const GdkRGBA *fg, double x, double y,
                      double w, double h, double maxv, int ticks,
                      int decimals, const char *suffix)
{
    for (int k = 0; k <= ticks; k++) {
        double v  = maxv * (double)k / (double)ticks;
        double yy = y + h - (v / maxv) * h;

        cairo_set_line_width(cr, 0.8);
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.10);
        cairo_move_to(cr, x, yy);
        cairo_line_to(cr, x + w, yy);
        cairo_stroke(cr);

        char b[32];

        g_snprintf(b, sizeof(b), "%.*f%s", decimals, v, suffix);
        gauge_font(cr, 9.5);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, b, &ext);
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.55);
        cairo_move_to(cr, x - ext.width - 6.0, yy + 3.5);
        cairo_show_text(cr, b);
    }
}

static void draw_thermal_series(cairo_t *cr, const CurveData *cd, double x,
                               double y, double w, double h, double maxv,
                               double r, double g, double b)
{
    if (cd == NULL || cd->count < 2 || !(maxv > 0.0))
        return;

    size_t cap = (size_t)((cd->count > FMON_HISTORY_POINTS)
                              ? FMON_HISTORY_POINTS
                              : cd->count) +
                 4;
    FmonChartPoint *pts = g_new0(FmonChartPoint, cap);

    uint64_t t0 = 0, t1 = 0;
    fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS, cd->count,
                          cd->index, &t0, &t1);
    uint64_t gap_us = cd->interval_us * 5u / 2u;

    size_t n = fmon_chart_polyline_at(
        cd->history, cd->time_us, FMON_HISTORY_POINTS, cd->count, cd->index,
        t0, t1, x, w, y, h, 0.0, maxv, (uint32_t)w, gap_us, pts, cap);

    if (n == 0) {
        g_free(pts);
        return;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    /* 渐变面积 */
    cairo_pattern_t *pat = cairo_pattern_create_linear(0.0, y, 0.0, y + h);

    cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, 0.28);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, 0.02);

    fmon_chart_cairo_fill(cr, pts, n, y + h);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    /* 线性折线：真实温度点之间直线连接 */
    cairo_set_line_width(cr, 1.35);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_source_rgba(cr, r, g, b, 0.95);
    fmon_chart_cairo_path(cr, pts, n);
    cairo_stroke(cr);

    cairo_restore(cr);
    g_free(pts);
}

/* 1/5/10 分钟滑动平均，三条线性曲线；用前缀和保证 O(n)。
 * 平均窗口内没有有效采样时断开折线，不把缺口当成 0。 */
static void draw_avg_curves(cairo_t *cr, const CurveData *cd, double x,
                            double y, double w, double h, double maxv,
                            int active)
{
    if (cd == NULL || cd->count < 2 || !(maxv > 0.0))
        return;

    uint32_t count = (cd->count > FMON_HISTORY_POINTS) ? FMON_HISTORY_POINTS
                                                       : cd->count;
    static double   prefix[FMON_HISTORY_POINTS + 1];
    static uint32_t counts[FMON_HISTORY_POINTS + 1];

    prefix[0] = 0.0;
    counts[0] = 0;

    for (uint32_t i = 0; i < count; i++) {
        double v = hist_at(cd, i);

        prefix[i + 1] = prefix[i] + (isfinite(v) ? v : 0.0);
        counts[i + 1] = counts[i] + (isfinite(v) ? 1u : 0u);
    }

    static const struct {
        uint32_t win;
        double   r, g, b;
    } k_bands[] = {
        { 60, 0.20, 0.55, 0.95 },   /* 1 分钟：蓝 */
        { 300, 0.20, 0.78, 0.45 },  /* 5 分钟：绿 */
        { 600, 0.95, 0.60, 0.20 },  /* 10 分钟：橙 */
    };

    uint32_t i0 = (count > 20) ? 15 : 0;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    for (int s = 0; s < 3; s++) {
        uint32_t win = k_bands[s].win;
        int      on  = (s == active);

        size_t cap = (size_t)count + 4;
        FmonChartPoint *pts = g_new0(FmonChartPoint, cap);
        size_t n = 0;
        bool gap = true;

        for (uint32_t i = i0; i < count; i++) {
            uint32_t from = (i + 1 > win) ? i + 1 - win : 0;

            if (from < i0)
                from = i0;

            uint32_t cnt = counts[i + 1] - counts[from];

            if (cnt == 0) {
                gap = true;
                continue;
            }

            double v = (prefix[i + 1] - prefix[from]) / (double)cnt;
            double xx = (count > 1)
                            ? x + w * (double)i / (double)(count - 1)
                            : x;

            pts[n].x = xx;
            pts[n].y = y + h - (v / maxv) * h;
            pts[n].gap = gap;
            pts[n].src = i;
            n++;
            gap = false;
        }

        if (n > 0 && cd->time_us != NULL) {
            uint64_t t0 = 0, t1 = 0;

            if (fmon_chart_time_range(cd->time_us, FMON_HISTORY_POINTS,
                                      count, cd->index, &t0, &t1))
                fmon_chart_retime(pts, n, cd->time_us, FMON_HISTORY_POINTS,
                                  count, cd->index, x, w, t0, t1,
                                  cd->interval_us * 5u / 2u);
        }

        if (n == 0) {
            g_free(pts);
            continue;
        }

        /* 高亮曲线：先铺一层浅色面积，突出“当前轮换”的那条 */
        if (on) {
            fmon_chart_cairo_fill(cr, pts, n, y + h);
            cairo_set_source_rgba(cr, k_bands[s].r, k_bands[s].g,
                                  k_bands[s].b, 0.12);
            cairo_fill(cr);
        }

        cairo_set_line_width(cr, on ? 2.4 : 1.2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
        cairo_set_source_rgba(cr, k_bands[s].r, k_bands[s].g, k_bands[s].b,
                              on ? 0.98 : 0.55);

        fmon_chart_cairo_path(cr, pts, n);
        cairo_stroke(cr);

        /* 末端点 + 数值：把计算后的平均负载直接标在曲线上 */
        double ex = pts[n - 1].x;
        double ey = pts[n - 1].y;

        cairo_set_source_rgba(cr, k_bands[s].r, k_bands[s].g, k_bands[s].b,
                              on ? 1.0 : 0.60);
        cairo_arc(cr, ex, ey, on ? 5.0 : 3.0, 0.0, 2.0 * G_PI);
        cairo_fill(cr);

        if (on) {
            double win_val = avg_window_value(cd, win);

            if (isfinite(win_val)) {
                char vb[32];

                g_snprintf(vb, sizeof(vb), "%.1f%%", win_val);
                gauge_font(cr, 12.0);

                cairo_text_extents_t ext;

                cairo_text_extents(cr, vb, &ext);
                cairo_move_to(cr, ex - ext.width - 10.0, ey - 10.0);
                cairo_show_text(cr, vb);
            }
        }

        g_free(pts);
    }

    cairo_restore(cr);
}

static double avg_window_value(const CurveData *cd, uint32_t win)
{
    if (cd == NULL || cd->count == 0)
        return NAN;

    uint32_t count = (cd->count > FMON_HISTORY_POINTS) ? FMON_HISTORY_POINTS
                                                       : cd->count;
    uint32_t i    = count - 1;
    uint32_t from = (i + 1 > win) ? i + 1 - win : 0;
    uint32_t warm = (count > 20) ? 15 : 0;

    if (from < warm)
        from = warm;

    double   acc = 0.0;
    uint32_t n   = 0;

    for (uint32_t k = from; k <= i; k++) {
        double v = hist_at(cd, k);

        if (isfinite(v)) {
            acc += v;
            n++;
        }
    }

    return (n > 0) ? acc / (double)n : NAN;
}

static double avg_window_max(const CurveData *cd, uint32_t win)
{
    if (cd == NULL || cd->count == 0)
        return 0.0;

    uint32_t count = (cd->count > FMON_HISTORY_POINTS) ? FMON_HISTORY_POINTS
                                                       : cd->count;
    static double   prefix[FMON_HISTORY_POINTS + 1];
    static uint32_t counts[FMON_HISTORY_POINTS + 1];

    prefix[0] = 0.0;
    counts[0] = 0;

    for (uint32_t i = 0; i < count; i++) {
        double v = hist_at(cd, i);

        prefix[i + 1] = prefix[i] + (isfinite(v) ? v : 0.0);
        counts[i + 1] = counts[i] + (isfinite(v) ? 1u : 0u);
    }

    uint32_t i0 = (count > 20) ? 15 : 0;
    double   mx = 0.0;

    for (uint32_t i = i0; i < count; i++) {
        uint32_t from = (i + 1 > win) ? i + 1 - win : 0;

        if (from < i0)
            from = i0;

        uint32_t cnt = counts[i + 1] - counts[from];

        if (cnt == 0)
            continue;

        double v = (prefix[i + 1] - prefix[from]) / (double)cnt;

        if (v > mx)
            mx = v;
    }

    /* 轴上限使用真实最大值，不再用 99.5% 分位裁掉尖峰。 */
    return mx;
}

static double nice_max(double v)
{
    if (!isfinite(v) || v <= 0.0)
        return 10.0;

    double m = ceil(v / 5.0) * 5.0;

    return (m < 10.0) ? 10.0 : m;
}

static void draw_thermometer(cairo_t *cr, const GdkRGBA *fg, double x,
                             double top, double h, double temp)
{
    double bw = 14.0;

    if (!isfinite(temp))
        temp = 0.0;

    double frac = temp / 100.0;

    if (frac < 0.0)
        frac = 0.0;
    if (frac > 1.0)
        frac = 1.0;

    /* 轨道 */
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.10);
    cairo_rectangle(cr, x, top, bw, h);
    cairo_fill(cr);

    /* 填充：按温度分级着色（绿 / 黄 / 红） */
    double fy;

    if (temp >= 85.0)
        cairo_set_source_rgba(cr, 0.88, 0.16, 0.14, 0.92);
    else if (temp >= 70.0)
        cairo_set_source_rgba(cr, 0.92, 0.65, 0.06, 0.92);
    else
        cairo_set_source_rgba(cr, 0.20, 0.78, 0.45, 0.92);

    fy = top + h * (1.0 - frac);
    cairo_rectangle(cr, x, fy, bw, h - (fy - top));
    cairo_fill(cr);

    /* 刻度 */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.30);

    for (int k = 0; k <= 4; k++) {
        double yy = top + h * (double)k / 4.0;

        cairo_move_to(cr, x + bw + 2.0, yy);
        cairo_line_to(cr, x + bw + 6.0, yy);
    }

    cairo_stroke(cr);
}


static void thermal_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                         gpointer data)
{
    (void)data;

    const CurveData *use  = gui_metric_curve(0);
    const CurveData *temp = gui_metric_curve(12);

    GdkRGBA fg;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);

    int half = (w - 24) / 2;

    gauge_font(cr, 13.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.90);
    cairo_move_to(cr, 12.0, 18.0);
    cairo_show_text(cr, "CPU 平均使用率（1 / 5 / 10 分钟滑动平均）");

    double lx = 48.0, ly = 50.0;
    double lw = half - 48.0 - 16.0;
    double lh = h - ly - 26.0;

    double amax = avg_window_max(use, 60);
    double m5   = avg_window_max(use, 300);
    double m10  = avg_window_max(use, 600);

    if (m5 > amax)
        amax = m5;
    if (m10 > amax)
        amax = m10;

    double nice = nice_max(amax);
    int    dec  = (nice / 5.0 < 5.0) ? 1 : 0;

    /* 每 3 秒轮换高亮 1 / 5 / 10 分钟其中一条 */
    int rot = (int)((g_get_monotonic_time() / G_TIME_SPAN_SECOND / 3) % 3);

    draw_axis(cr, &fg, lx, ly, lw, lh, nice, 5, dec, "%");
    draw_avg_curves(cr, use, lx, ly, lw, lh, nice, rot);

    /* 三色图例 + 当前值（精确到 0.1%） */
    static const double leg[3][3] = {
        { 0.20, 0.55, 0.95 },
        { 0.20, 0.78, 0.45 },
        { 0.95, 0.60, 0.20 },
    };
    static const uint32_t leg_win[3] = { 60, 300, 600 };
    static const char *const leg_txt[3] = { "1 分钟", "5 分钟", "10 分钟" };
    /* 图例单独占第二行，避免和上面的标题重叠 */
    double lgd_x = 12.0;

    gauge_font(cr, 9.5);

    for (int i = 0; i < 3; i++) {
        cairo_set_source_rgba(cr, leg[i][0], leg[i][1], leg[i][2], 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, lgd_x, 35.0);
        cairo_line_to(cr, lgd_x + 14.0, 35.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue,
                              (i == rot) ? 0.95 : 0.55);

        char lb[48];

        double leg_val = avg_window_value(use, leg_win[i]);

        if (isfinite(leg_val))
            g_snprintf(lb, sizeof(lb), "%s %.1f%%", leg_txt[i], leg_val);
        else
            g_snprintf(lb, sizeof(lb), "%s —", leg_txt[i]);

        cairo_move_to(cr, lgd_x + 18.0, 38.0);
        cairo_show_text(cr, lb);
        lgd_x += 88.0;
    }

    double rx0 = 12.0 + half;

    gauge_font(cr, 13.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.90);
    cairo_move_to(cr, rx0 + 4.0, 18.0);
    cairo_show_text(cr, "CPU 温度");

    uint32_t temp_n = (temp != NULL && temp->count > FMON_HISTORY_POINTS)
                          ? FMON_HISTORY_POINTS
                          : (temp != NULL ? temp->count : 0);
    double cur_temp = (temp_n > 0) ? hist_at(temp, temp_n - 1) : NAN;

    draw_thermometer(cr, &fg, rx0 + 8.0, ly + 6.0, lh - 22.0, cur_temp);

    /* 温度计柱 + 刻度标签占位，绘图区右移，避免刻度被柱体盖住 */
    double tx = rx0 + 84.0;
    double twd = half - 84.0 - 16.0;

    /* 固定 0-100°C 基准；真实温度更高时轴自动扩展，不裁掉尖峰。 */
    double tmax = 100.0;
    double tlo = 0.0, thi = 0.0;

    if (temp != NULL &&
        fmon_chart_history_range(temp->history, FMON_HISTORY_POINTS,
                                 temp->count, temp->index, &tlo, &thi) &&
        thi > tmax)
        tmax = ceil(thi / 20.0) * 20.0;

    draw_axis(cr, &fg, tx, ly, twd, lh, tmax, 4, 0, "°C");
    draw_thermal_series(cr, temp, tx, ly, twd, lh, tmax, 0.92, 0.30, 0.25);

    /* 大号当前温度：放在温度图右上角空白处 */
    {
        char b[32];

        gauge_font(cr, 26.0);

        if (isfinite(cur_temp))
            g_snprintf(b, sizeof(b), "%.1f°C", cur_temp);
        else
            g_snprintf(b, sizeof(b), "—");

        cairo_text_extents_t ext;

        cairo_text_extents(cr, b, &ext);

        if (isfinite(cur_temp) && cur_temp >= 85.0)
            cairo_set_source_rgba(cr, 0.88, 0.16, 0.14, 0.95);
        else if (isfinite(cur_temp) && cur_temp >= 70.0)
            cairo_set_source_rgba(cr, 0.92, 0.65, 0.06, 0.95);
        else if (isfinite(cur_temp))
            cairo_set_source_rgba(cr, 0.92, 0.30, 0.25, 0.95);
        else
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.55);

        cairo_move_to(cr, tx + twd - ext.width - 8.0, ly + 26.0);
        cairo_show_text(cr, b);
    }
}

GtkWidget *gui_cpu_thermal_card(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_add_css_class(card, "page-card");

    GtkWidget *t = gtk_label_new(_("CPU 负载与温度"));

    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(card), t);

    GtkWidget *s =
        gtk_label_new(
            _("左：CPU 使用率的 1/5/10 分钟平均值（%）· 右：温度曲线 + CPU 温度计"));

    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_box_append(GTK_BOX(card), s);

    s_thermal_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_thermal_area, -1, 300);
    gtk_widget_set_hexpand(s_thermal_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_thermal_area),
                                   thermal_draw, NULL, NULL);
    gtk_box_append(GTK_BOX(card), s_thermal_area);

    return card;
}

void gui_cpu_thermal_update(void)
{
    if (s_thermal_area == NULL || !gtk_widget_get_mapped(s_thermal_area))
        return;

    gtk_widget_queue_draw(s_thermal_area);
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

    gtk_label_set_text(GTK_LABEL(s_val[V_VIRT]),
                       cs->virt_enabled ? _("已启用（vmm）") : _("未启用"));

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



























