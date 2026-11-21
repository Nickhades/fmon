/*
 * gui_cpu_gauge.c - CPU 使用率仪表与健康摘要
 */

#include "gui_cpu_gauge.h"

#include "alerts.h"
#include "cpu_health.h"
#include "cpu_curve_util.h"
#include "cpu_stats.h"
#include "gui_chart.h"
#include "gui_pages.h"
#include "gui_state.h"
#include "palette.h"
#include "ui_metrics.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>

/* ===========================================================================
 * 半圆仪表卡片
 * ========================================================================= */

static GtkWidget *s_gauge_area;
static GtkWidget *s_health;
static double     s_g_cur;
static double     s_g_avg;
static double     s_g_load[3];
static unsigned   s_g_ncpu;

void gui_cpu_gauge_font(cairo_t *cr, double size)
{
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, size);
}

static void gauge_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer data)
{
    (void)data;

    cairo_save(cr);
    cairo_rectangle(cr, 0.0, 0.0, (double)w, (double)h);
    cairo_clip(cr);

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

    double cpu_color[4];

    fmon_palette_get(FMON_COLOR_CPU, cpu_color);

    /* 终端风格：直角分段条，不用圆弧仪表。 */
    double bx = 16.0;
    double bw = MIN(w * 0.30, 300.0);
    double bh = MAX(10.0, r * 0.09);
    double by = cy - r * 0.22;

    if (bw < 40.0)
        bw = 40.0;

    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, cpu_color[0], cpu_color[1], cpu_color[2]);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_stroke(cr);

    /* 连续刻度条：低使用率只亮很短一段，不会因为空槽轮廓
     * 看起来像“整条都满了”。 */
    cairo_set_source_rgb(cr, cpu_color[0], cpu_color[1], cpu_color[2]);
    cairo_rectangle(cr, bx + 1.0, by + 1.0, (bw - 2.0) * frac, bh - 2.0);
    cairo_fill(cr);

    /* 25/50/75% 细刻度，仅作视觉参照，不填充空槽。 */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, cpu_color[0] * 0.35, cpu_color[1] * 0.35,
                         cpu_color[2] * 0.35);
    for (int i = 1; i < 4; i++) {
        double tick = bx + bw * (double)i / 4.0;

        cairo_move_to(cr, tick, by + 1.0);
        cairo_line_to(cr, tick, by + bh - 1.0);
        cairo_stroke(cr);
    }

    /* CPU 告警阈值：在分段条上画红色竖线。 */
    const AlertSettings *alerts = alerts_current();

    if (alerts != NULL && alerts->enabled &&
        alerts->cpu_pct > 0.0 && alerts->cpu_pct <= 100.0) {
        double tx = bx + bw * alerts->cpu_pct / 100.0;

        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, tx, by - 3.0);
        cairo_line_to(cr, tx, by + bh + 3.0);
        cairo_stroke(cr);
    }

    char txt[96];
    cairo_text_extents_t ext;

    gui_cpu_gauge_font(cr, r * 0.30);
    g_snprintf(txt, sizeof(txt), "%.1f%%", s_g_cur);
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgba(cr, cpu_color[0], cpu_color[1], cpu_color[2], 0.95);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, cy - r * 0.34);
    cairo_show_text(cr, txt);

    gui_cpu_gauge_font(cr, r * 0.13);
    g_snprintf(txt, sizeof(txt), "近 5 分钟平均 %.1f%%", s_g_avg);
    cairo_text_extents(cr, txt, &ext);
    cairo_set_source_rgba(cr, cpu_color[0], cpu_color[1], cpu_color[2], 0.70);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing,
                  by + bh + 20.0);
    cairo_show_text(cr, txt);

    /* 右侧：1 / 5 / 15 分钟平均负载 */
    GdkRGBA fg;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);

    double tx = w * 0.42;

    gui_cpu_gauge_font(cr, 13.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.60);
    cairo_move_to(cr, tx, cy - r * 0.52);
    if (s_g_ncpu > 0) {
        char head[192];

        if (w < 700) {
            g_snprintf(head, sizeof(head),
                       "系统平均负载（每线程 %.2f/%.2f/%.2f）",
                       s_g_load[0] / (double)s_g_ncpu,
                       s_g_load[1] / (double)s_g_ncpu,
                       s_g_load[2] / (double)s_g_ncpu);
        } else {
            g_snprintf(head, sizeof(head),
                       "系统平均负载（可运行任务数 · 每线程 %.2f / %.2f / %.2f）",
                       s_g_load[0] / (double)s_g_ncpu,
                       s_g_load[1] / (double)s_g_ncpu,
                       s_g_load[2] / (double)s_g_ncpu);
        }
        cairo_show_text(cr, head);
    } else {
        cairo_show_text(cr, "系统平均负载（可运行任务数平均值）");
    }

    gui_cpu_gauge_font(cr, w < 560 ? 20.0 : 24.0);
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

    gui_cpu_gauge_font(cr, 12.0);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.55);
    cairo_move_to(cr, tx, cy - r * 0.02);
    if (w < 700)
        cairo_show_text(cr, "1 / 5 / 15 分钟（任务数）");
    else
        cairo_show_text(
            cr, "1 分钟 / 5 分钟 / 15 分钟（单位：任务数；每线程 <1 不排队）");

    cairo_restore(cr);
}

static GtkWidget *s_gauge_header_right;
static GtkWidget *s_gauge_body;


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
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_add_css_class(card, "page-card");

    GtkWidget *t = gtk_label_new(_("CPU 使用率"));

    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_widget_set_halign(t, GTK_ALIGN_START);

    GtkWidget *s = gtk_label_new(_("当前值 · 历史平均 · 1/5/15 分钟负载"));

    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_widget_set_halign(s, GTK_ALIGN_START);

    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_hexpand(title_box, TRUE);
    gtk_box_append(GTK_BOX(title_box), t);
    gtk_box_append(GTK_BOX(title_box), s);

    s_gauge_header_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);
    gtk_widget_set_halign(s_gauge_header_right, GTK_ALIGN_END);
    gtk_widget_set_valign(s_gauge_header_right, GTK_ALIGN_START);

    s_health = gtk_label_new(_("设备正常"));
    gtk_widget_add_css_class(s_health, "health-ok");
    gtk_label_set_xalign(GTK_LABEL(s_health), 1.0f);
    gtk_widget_set_halign(s_health, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(s_gauge_header_right), s_health);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_4);

    gtk_box_append(GTK_BOX(header), title_box);
    gtk_box_append(GTK_BOX(header), s_gauge_header_right);
    gtk_box_append(GTK_BOX(card), header);

    s_gauge_area = gtk_drawing_area_new();
    /* 保留仪表+负载文字的最小宽度；窗口变窄时不再把文字压进状态区。 */
    gtk_widget_set_size_request(s_gauge_area, 440, 180);
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
    /* FlowBox：宽屏并排，窄屏自动把状态块换到仪表下方。 */
    GtkWidget *body = gtk_flow_box_new();

    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(body), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(body), 2);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(body), 1);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(body), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(body), 8);
    gtk_widget_set_hexpand(body, TRUE);
    gtk_flow_box_append(GTK_FLOW_BOX(body), s_gauge_area);
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
                                   ? cpu_curve_avg_window_value(use_cd, 60)
                                   : cur;
    double           tcur    = (temp_cd != NULL && temp_cd->count > 0)
                                   ? cpu_curve_hist_at(temp_cd, temp_cd->count - 1)
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
