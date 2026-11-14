/*
 * gui_page_build.c - 页面骨架构建器（指标页 / 组合页 / 滚动包装）
 */

#include "gui_page_build.h"

#include "gui_page_state.h"
#include "gui_chart.h"
#include "gui_pages.h"
#include "gui_state.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

GtkWidget *gui_page_scrolled(GtkWidget *child)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
    /* 横向用 AUTOMATIC：宽表格只在页内滚动，不把主窗口撑大导致整窗移位 */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_vexpand(child, TRUE);
    gtk_widget_set_valign(child, GTK_ALIGN_FILL);
    return scroll;
}

/* ===========================================================================
 * 数据 → 卡片
 * ========================================================================= */

GtkWidget *gui_combo_page_new(const char *title, const char *subtitle,
                                    ComboChart *chart,
                                    const char *const *rows, int nrows,
                                    ComboPage *page)
{
    page->nvals = 0;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_widget_set_halign(t, GTK_ALIGN_START);

    GtkWidget *s = gtk_label_new(subtitle);
    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    page->sub = s;

    /* 标题行：左边标题/副标题，右边留给页面自己的摘要（内存页放内存条信息），
     * 这样标题区右侧的空白不会被浪费，也不用把内容挤到曲线下面。 */
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_hexpand(title_box, TRUE);
    gtk_box_append(GTK_BOX(title_box), t);
    gtk_box_append(GTK_BOX(title_box), s);

    page->header_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(page->header_right, GTK_ALIGN_END);
    gtk_widget_set_valign(page->header_right, GTK_ALIGN_START);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

    gtk_box_append(GTK_BOX(header), title_box);
    gtk_box_append(GTK_BOX(header), page->header_right);

    /* 标题、副标题与曲线共用一张大卡片 */
    GtkWidget *page_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(page_card, "page-card");
    gtk_box_append(GTK_BOX(page_card), header);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 320);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), combo_chart_draw,
                                   chart, NULL);
    page->area = area;

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(combo_chart_motion), chart);
    g_signal_connect(motion, "leave", G_CALLBACK(combo_chart_leave), chart);
    gtk_widget_add_controller(area, motion);

    gtk_box_append(GTK_BOX(page_card), area);
    gtk_box_append(GTK_BOX(vbox), page_card);

    /* 下方数据：每个分类一张横向卡片，卡片有表头，数值用大字号 */
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(cards, 8);
    gtk_widget_set_hexpand(cards, TRUE);

    GtkWidget *card  = NULL;
    GtkWidget *rowsv = NULL;

    for (int i = 0; i < nrows; i++) {
        if (strncmp(rows[i], "##", 2) == 0) {
            if (card != NULL)
                gtk_box_append(GTK_BOX(cards), card);

            card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_add_css_class(card, "detail-card");
            gtk_widget_set_hexpand(card, TRUE);

            GtkWidget *sec = gtk_label_new(rows[i] + 2);
            gtk_widget_add_css_class(sec, "detail-title");
            gtk_label_set_xalign(GTK_LABEL(sec), 0.0f);
            gtk_widget_set_halign(sec, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(card), sec);

            rowsv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_box_append(GTK_BOX(card), rowsv);
            continue;
        }

        if (rowsv == NULL) {
            card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_add_css_class(card, "detail-card");
            gtk_widget_set_hexpand(card, TRUE);
            rowsv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_box_append(GTK_BOX(card), rowsv);
        }

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(row, 1);
        gtk_widget_set_margin_bottom(row, 1);

        GtkWidget *k = gtk_label_new(rows[i]);
        gtk_widget_add_css_class(k, "detail-key");
        gtk_label_set_xalign(GTK_LABEL(k), 0.0f);

        GtkWidget *val = gtk_label_new("—");
        gtk_widget_add_css_class(val, "detail-value");
        gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
        gtk_widget_set_hexpand(val, TRUE);

        gtk_box_append(GTK_BOX(row), k);
        gtk_box_append(GTK_BOX(row), val);
        gtk_box_append(GTK_BOX(rowsv), row);

        if (page->nvals < 12)
            page->vals[page->nvals++] = val;
    }

    if (card != NULL)
        gtk_box_append(GTK_BOX(cards), card);

    gtk_box_append(GTK_BOX(vbox), cards);
    return vbox;
}

/* 曲线悬停：记录鼠标位置供 gui_chart 绘制 */
static void page_chart_motion(GtkEventControllerMotion *ctrl, double x,
                              double y, gpointer data)
{
    (void)y;

    CurveData *cd = data;

    cd->hovering = TRUE;
    cd->hover_x  = x;

    gtk_widget_queue_draw(gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(ctrl)));
}

static void page_chart_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    CurveData *cd = data;

    cd->hovering = FALSE;

    gtk_widget_queue_draw(gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(ctrl)));
}

GtkWidget *gui_metric_page_new(MetricPage *mp, int idx, const char *title,
                                     const char *subtitle,
                                     const char *const *keys, int nkeys,
                                     void (*update)(MetricPage *),
                                     gboolean compact)
{
    mp->update = update;

    int nv = 0;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "group-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_widget_set_halign(t, GTK_ALIGN_START);

    GtkWidget *s = gtk_label_new(subtitle);
    gtk_widget_add_css_class(s, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    mp->sub = s;

    /* 标题、副标题与曲线共用一张大卡片 */
    GtkWidget *page_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(page_card, "page-card");

    if (compact) {
        /* 数值放在标题行右侧：左边标题/副标题，右边 2~3 列键值对 */
        GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        gtk_widget_set_hexpand(title_box, TRUE);
        gtk_box_append(GTK_BOX(title_box), t);
        gtk_box_append(GTK_BOX(title_box), s);

        int cols = (nkeys >= 5) ? 3 : 2;

        GtkWidget *grid = gtk_grid_new();

        gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
        gtk_widget_set_halign(grid, GTK_ALIGN_END);
        gtk_widget_set_valign(grid, GTK_ALIGN_START);

        for (int i = 0; i < nkeys; i++) {
            int row = i / cols;
            int col = i % cols;

            GtkWidget *k = gtk_label_new(keys[i]);
            gtk_widget_add_css_class(k, "detail-key");
            gtk_label_set_xalign(GTK_LABEL(k), 1.0f);

            GtkWidget *val = gtk_label_new("—");
            gtk_widget_add_css_class(val, "detail-value");
            gtk_label_set_xalign(GTK_LABEL(val), 0.0f);

            gtk_grid_attach(GTK_GRID(grid), k, col * 2, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), val, col * 2 + 1, row, 1, 1);

            if (nv < 12)
                mp->vals[nv++] = val;
        }

        GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

        gtk_box_append(GTK_BOX(header), title_box);
        gtk_box_append(GTK_BOX(header), grid);
        gtk_box_append(GTK_BOX(page_card), header);
    } else {
        gtk_box_append(GTK_BOX(page_card), t);
        gtk_box_append(GTK_BOX(page_card), s);
    }

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 320);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), gui_chart_draw,
                                   gui_metric_curve(idx), NULL);
    mp->area = area;

    /* 鼠标悬停显示精确数据（与卡片一致） */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(page_chart_motion),
                     gui_metric_curve(idx));
    g_signal_connect(motion, "leave", G_CALLBACK(page_chart_leave),
                     gui_metric_curve(idx));
    gtk_widget_add_controller(area, motion);

    gtk_box_append(GTK_BOX(page_card), area);
    gtk_box_append(GTK_BOX(vbox), page_card);

    /* 紧凑模式：数值已经在标题行，底部不再占一块 */
    if (compact) {
        update(mp);
        return vbox;
    }

    GtkWidget *detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(detail, "metric-detail");
    gtk_widget_set_margin_top(detail, 4);

    for (int i = 0; i < nkeys; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_widget_set_margin_start(row, 14);
        gtk_widget_set_margin_end(row, 14);
        gtk_widget_set_margin_top(row, 3);
        gtk_widget_set_margin_bottom(row, 3);

        GtkWidget *k = gtk_label_new(keys[i]);
        gtk_widget_add_css_class(k, "small-text");
        gtk_label_set_xalign(GTK_LABEL(k), 0.0f);
        gtk_widget_set_size_request(k, 170, -1);

        GtkWidget *val = gtk_label_new("—");
        gtk_widget_add_css_class(val, "detail-value");
        gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
        gtk_widget_set_hexpand(val, TRUE);

        gtk_box_append(GTK_BOX(row), k);
        gtk_box_append(GTK_BOX(row), val);
        gtk_box_append(GTK_BOX(detail), row);

        mp->vals[i] = val;
    }

    gtk_box_append(GTK_BOX(vbox), detail);

    update(mp);
    return vbox;
}

