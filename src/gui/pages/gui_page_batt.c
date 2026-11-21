/*
 * gui_page_batt.c - 电池健康与供电细节
 *
 * 数据来自 acpiconf -i 0（普通用户可读），每 60 秒最多读一次；
 * 不同笔记本 ACPI 暴露程度不同，缺失项显示“不可用”，不编造。
 */

#include "gui_page_batt.h"
#include "ui_metrics.h"

#include <glib/gi18n.h>

#include "fmon_child.h"
#include "gui_state.h"
#include "theme.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int      design_mwh;
    int      full_mwh;
    int      cycles;
    int      design_mv;
    int      present_mv;
    int      rate_mw;
    char     state[32];
    char     model[48];
    char     oem[32];
    char     remain[48];
    gboolean valid;
    gboolean has_cycles;
    gint64   last_read;
} BattInfo;

static BattInfo g_bi;

static GtkWidget *s_design;
static GtkWidget *s_full;
static GtkWidget *s_health;
static GtkWidget *s_cycles;
static GtkWidget *s_voltage;
static GtkWidget *s_current;
static GtkWidget *s_power;
static GtkWidget *s_time;
static GtkWidget *s_temp;
static GtkWidget *s_model;
static GtkWidget *s_thresh;
static GtkWidget *s_chart;

#define BATT_HIST_PTS 600

static double s_hist_pct[BATT_HIST_PTS];
static double s_hist_pwr[BATT_HIST_PTS];
static int    s_hist_idx;
static int    s_hist_count;

static void batt_hist_push(double pct, double pwr)
{
    if (!isfinite(pct) || !isfinite(pwr))
        return;

    s_hist_pct[s_hist_idx] = pct;
    s_hist_pwr[s_hist_idx] = pwr;
    s_hist_idx = (s_hist_idx + 1) % BATT_HIST_PTS;

    if (s_hist_count < BATT_HIST_PTS)
        s_hist_count++;
}

static void batt_hist_get(int i, double *pct, double *pwr)
{
    int start = (s_hist_count < BATT_HIST_PTS) ? 0
                                               : s_hist_idx;
    int idx = (start + i) % BATT_HIST_PTS;

    *pct = s_hist_pct[idx];
    *pwr = s_hist_pwr[idx];
}

static void draw_batt_chart(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                            gpointer data)
{
    (void)area;
    (void)data;

    double axis[4], grid[4];

    fmon_theme_axis_color(axis);
    fmon_theme_grid_color(grid, FALSE);

    double ml = 46.0, mr = 52.0, mt = 10.0, mb = 20.0;
    double pw = w - ml - mr;
    double ph = h - mt - mb;

    if (pw < 20 || ph < 20)
        return;

    if (s_hist_count < 2) {
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.7);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, ml + 8.0, mt + ph / 2.0);
        cairo_show_text(cr, _("打开电池页后开始记录（约 20 分钟窗口）"));
        return;
    }

    double maxw = 1.0;

    for (int i = 0; i < s_hist_count; i++) {
        double pct, pwr;

        batt_hist_get(i, &pct, &pwr);
        if (fabs(pwr) > maxw)
            maxw = fabs(pwr);
    }

    maxw = ceil(maxw);

    double zero = mt + ph / 2.0;

    cairo_set_line_width(cr, 1.0);

    for (int i = 0; i <= 2; i++) {
        double y = mt + ph * (double)i / 2.0;

        cairo_set_source_rgba(cr, grid[0], grid[1], grid[2],
                              (i == 1) ? 0.9 : 0.5);
        cairo_move_to(cr, ml, y);
        cairo_line_to(cr, ml + pw, y);
        cairo_stroke(cr);
    }

    /* 功率柱：充电向上（绿），放电向下（红） */
    double bwid = pw / s_hist_count;

    for (int i = 0; i < s_hist_count; i++) {
        double pct, pwr;

        batt_hist_get(i, &pct, &pwr);

        if (fabs(pwr) < 0.01)
            continue;

        double x = ml + pw * (double)i / s_hist_count;
        double hgt = (pwr / maxw) * (ph / 2.0);

        if (pwr >= 0)
            cairo_set_source_rgb(cr, 0.00, 1.00, 0.00);
        else
            cairo_set_source_rgb(cr, 1.00, 0.00, 0.00);

        if (pwr >= 0)
            cairo_rectangle(cr, x, zero - hgt, bwid * 0.8, hgt);
        else
            cairo_rectangle(cr, x, zero, bwid * 0.8, -hgt);

        cairo_fill(cr);
    }

    /* 电量折线（0-100%） */
    cairo_set_source_rgb(cr, 0.00, 1.00, 1.00);
    cairo_set_line_width(cr, 1.2);

    double last_y = 0.0;

    for (int i = 0; i < s_hist_count; i++) {
        double pct, pwr;

        batt_hist_get(i, &pct, &pwr);

        double x = ml + pw * (double)i / (double)(s_hist_count - 1);
        double y = mt + ph - (pct / 100.0) * ph;

        if (i == 0) {
            cairo_move_to(cr, x, y);
        } else {
            cairo_line_to(cr, x, last_y);
            cairo_line_to(cr, x, y);
        }
        last_y = y;
    }

    cairo_stroke(cr);

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.0);
    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.8);

    char buf[32];

    for (int i = 0; i <= 2; i++) {
        double y = mt + ph * (double)i / 2.0;

        g_snprintf(buf, sizeof(buf), "%d%%", 100 - i * 50);
        cairo_move_to(cr, 4.0, y + 3.0);
        cairo_show_text(cr, buf);

        double pv = maxw * (1.0 - i);

        g_snprintf(buf, sizeof(buf), "%+.1fW", pv);
        cairo_move_to(cr, ml + pw + 6.0, y + 3.0);
        cairo_show_text(cr, buf);
    }

    cairo_set_font_size(cr, 8.5);
    cairo_move_to(cr, ml, h - 4.0);
    cairo_show_text(cr, _("蓝：电量（0-100%） 绿/红：充电/放电功率"));
}

static void trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));

    char *e = s + strlen(s);

    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                     e[-1] == '\t'))
        *--e = '\0';
}

static void read_acpiconf(void)
{
    const char *const argv[] = { "/usr/sbin/acpiconf", "-i", "0", NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 64 * 1024, 2000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        memset(&g_bi, 0, sizeof(g_bi));
        g_bi.last_read = (gint64)time(NULL);
        return;
    }

    BattInfo bi;
    memset(&bi, 0, sizeof(bi));

    char *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');

        if (colon == NULL)
            continue;

        *colon = '\0';

        char *key = line;
        char *val = colon + 1;

        trim(key);
        trim(val);

        if (strcmp(key, "Design capacity") == 0)
            bi.design_mwh = atoi(val);
        else if (strcmp(key, "Last full capacity") == 0)
            bi.full_mwh = atoi(val);
        else if (strcmp(key, "Cycle Count") == 0) {
            bi.cycles = atoi(val);
            bi.has_cycles = TRUE;
        } else if (strcmp(key, "Design voltage") == 0)
            bi.design_mv = atoi(val);
        else if (strcmp(key, "Present voltage") == 0)
            bi.present_mv = atoi(val);
        else if (strcmp(key, "Present rate") == 0)
            bi.rate_mw = atoi(val);
        else if (strcmp(key, "Remaining time") == 0)
            g_strlcpy(bi.remain, val, sizeof(bi.remain));
        else if (strcmp(key, "State") == 0)
            g_strlcpy(bi.state, val, sizeof(bi.state));
        else if (strcmp(key, "Model number") == 0)
            g_strlcpy(bi.model, val, sizeof(bi.model));
        else if (strcmp(key, "OEM info") == 0)
            g_strlcpy(bi.oem, val, sizeof(bi.oem));
    }

    fmon_child_result_free(&res);

    bi.valid = (bi.design_mwh > 0 && bi.full_mwh > 0);
    bi.last_read = (gint64)time(NULL);
    g_bi = bi;
}

static GtkWidget *row_key(const char *text)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_widget_add_css_class(l, "detail-key");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_halign(l, GTK_ALIGN_START);

    return l;
}

static GtkWidget *row_val(void)
{
    GtkWidget *l = gtk_label_new("—");

    gtk_widget_add_css_class(l, "detail-value");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_halign(l, GTK_ALIGN_START);

    return l;
}

GtkWidget *gui_batt_detail_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);

    GtkWidget *t = gtk_label_new(_("电池健康与供电"));

    gtk_widget_add_css_class(t, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(box), t);

    s_chart = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_chart, -1, 150);
    gtk_widget_set_hexpand(s_chart, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_chart), draw_batt_chart,
                                   NULL, NULL);
    gtk_box_append(GTK_BOX(box), s_chart);

    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(grid), FMON_SPACE_1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), FMON_SPACE_4);

    int r = 0;

#define ADD_ROW(k1, v1, k2, v2)                                            \
    do {                                                                   \
        GtkWidget *a = row_key(k1);                                        \
        gtk_grid_attach(GTK_GRID(grid), a, 0, r, 1, 1);                    \
        gtk_grid_attach(GTK_GRID(grid), v1, 1, r, 1, 1);                   \
        if ((k2) != NULL) {                                                \
            GtkWidget *c = row_key(k2);                                   \
            gtk_grid_attach(GTK_GRID(grid), c, 2, r, 1, 1);                \
            gtk_grid_attach(GTK_GRID(grid), v2, 3, r, 1, 1);               \
        }                                                                  \
        r++;                                                               \
    } while (0)

    s_design  = row_val();
    s_full    = row_val();
    s_health  = row_val();
    s_cycles  = row_val();
    s_voltage = row_val();
    s_current = row_val();
    s_power   = row_val();
    s_time    = row_val();
    s_temp    = row_val();
    s_model   = row_val();
    s_thresh  = row_val();

    ADD_ROW(_("设计容量"), s_design, _("满充容量"), s_full);
    ADD_ROW(_("健康度"), s_health, _("循环次数"), s_cycles);
    ADD_ROW(_("电压"), s_voltage, _("电流 / 功率"), s_current);
    ADD_ROW(_("剩余时间"), s_time, _("电池温度"), s_temp);
    ADD_ROW(_("型号 / OEM"), s_model, _("充电阈值"), s_thresh);

#undef ADD_ROW

    gtk_box_append(GTK_BOX(box), grid);
    return box;
}

void gui_batt_detail_update(void)
{
    if (s_design == NULL)
        return;

    gint64 now = (gint64)time(NULL);

    if (g_bi.last_read == 0 || now - g_bi.last_read >= 60)
        read_acpiconf();

    /* 双轴历史：电量 + 带符号功率（充电正、放电负） */
    const FmonShmData *snap = gui_state_snapshot();
    double pwr = 0.0;

    if (g_bi.valid) {
        if (strcmp(g_bi.state, "charging") == 0)
            pwr = g_bi.rate_mw / 1000.0;
        else if (strcmp(g_bi.state, "discharging") == 0)
            pwr = -g_bi.rate_mw / 1000.0;
    }

    if (fabs(pwr) < 0.001 && snap->battery_present) {
        double d = snap->battery_discharge_w;

        if ((snap->battery_flags & FMON_BATT_CHARGING) != 0)
            pwr = fabs(d);
        else if ((snap->battery_flags & FMON_BATT_DISCHARGING) != 0)
            pwr = -fabs(d);
    }

    if (snap->battery_present) {
        batt_hist_push(snap->battery_pct, pwr);

        if (s_chart != NULL)
            gtk_widget_queue_draw(s_chart);
    }

    if (!g_bi.valid) {
        const char *none = _("不可用（ACPI 未暴露）");

        gtk_label_set_text(GTK_LABEL(s_design), none);
        gtk_label_set_text(GTK_LABEL(s_full), none);
        gtk_label_set_text(GTK_LABEL(s_health), none);
        gtk_label_set_text(GTK_LABEL(s_cycles), none);
        gtk_label_set_text(GTK_LABEL(s_voltage), "—");
        gtk_label_set_text(GTK_LABEL(s_current), "—");
        gtk_label_set_text(GTK_LABEL(s_power), "—");
        gtk_label_set_text(GTK_LABEL(s_time), "—");
        gtk_label_set_text(GTK_LABEL(s_temp), none);
        gtk_label_set_text(GTK_LABEL(s_model), "—");
        gtk_label_set_text(GTK_LABEL(s_thresh), "—");
        return;
    }

    char b[96];

    g_snprintf(b, sizeof(b), "%.1f Wh", g_bi.design_mwh / 1000.0);
    gtk_label_set_text(GTK_LABEL(s_design), b);

    g_snprintf(b, sizeof(b), "%.1f Wh", g_bi.full_mwh / 1000.0);
    gtk_label_set_text(GTK_LABEL(s_full), b);

    double health = 100.0 * (double)g_bi.full_mwh / (double)g_bi.design_mwh;

    g_snprintf(b, sizeof(b), _("%.1f %%（损耗 %.1f %%）"), health,
               100.0 - health);
    gtk_label_set_text(GTK_LABEL(s_health), b);

    gtk_widget_remove_css_class(s_health, "health-ok");
    gtk_widget_remove_css_class(s_health, "health-warn");
    gtk_widget_remove_css_class(s_health, "health-bad");

    if (health < 80.0)
        gtk_widget_add_css_class(s_health, "health-bad");
    else if (health < 90.0)
        gtk_widget_add_css_class(s_health, "health-warn");
    else
        gtk_widget_add_css_class(s_health, "health-ok");

    if (g_bi.has_cycles)
        g_snprintf(b, sizeof(b), _("%d 次"), g_bi.cycles);
    else
        g_strlcpy(b, _("不可用"), sizeof(b));
    gtk_label_set_text(GTK_LABEL(s_cycles), b);

    g_snprintf(b, sizeof(b), "%.2f V / 设计 %.2f V",
               g_bi.present_mv / 1000.0, g_bi.design_mv / 1000.0);
    gtk_label_set_text(GTK_LABEL(s_voltage), b);

    double amps = (g_bi.present_mv > 0)
                      ? (double)g_bi.rate_mw / (double)g_bi.present_mv
                      : 0.0;

    g_snprintf(b, sizeof(b), "%.2f A / %.2f W", amps,
               g_bi.rate_mw / 1000.0);
    gtk_label_set_text(GTK_LABEL(s_current), b);

    const char *state = g_bi.state;

    if (strcmp(state, "charging") == 0)
        g_snprintf(b, sizeof(b), _("%.2f W（充电中）"),
                   g_bi.rate_mw / 1000.0);
    else if (strcmp(state, "discharging") == 0)
        g_snprintf(b, sizeof(b), _("%.2f W（放电中）"),
                   g_bi.rate_mw / 1000.0);
    else if (strcmp(state, "full") == 0)
        g_strlcpy(b, _("已充满"), sizeof(b));
    else
        g_snprintf(b, sizeof(b), "%.2f W", g_bi.rate_mw / 1000.0);

    gtk_label_set_text(GTK_LABEL(s_power), b);

    if (g_bi.remain[0] != '\0' && strcmp(g_bi.remain, "unknown") != 0)
        gtk_label_set_text(GTK_LABEL(s_time), g_bi.remain);
    else
        gtk_label_set_text(GTK_LABEL(s_time), _("未知（ACPI 未报告）"));

    gtk_label_set_text(GTK_LABEL(s_temp),
                       _("不可用（ACPI 未暴露）"));

    if (g_bi.model[0] != '\0') {
        g_snprintf(b, sizeof(b), "%s / %s", g_bi.model,
                   (g_bi.oem[0] != '\0') ? g_bi.oem : "?");
        gtk_label_set_text(GTK_LABEL(s_model), b);
    } else {
        gtk_label_set_text(GTK_LABEL(s_model), "—");
    }

    gtk_label_set_text(GTK_LABEL(s_thresh), _("未设置 / 不支持"));
}
