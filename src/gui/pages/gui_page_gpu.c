/*
 * gui_page_gpu.c - 每张显卡一个独立页面
 *
 * 页面结构：标题（设备名）+ 副标题 + 使用率曲线（带悬停读数）+
 * 显卡详情卡片。页面数量按实际检测结果动态显示。
 */

#include "gui_page_gpu.h"
#include "ui_metrics.h"

#include "gui_chart.h"
#include "gui_state.h"
#include "palette.h"
#include "gui_util.h"
#include "gui_widgets.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>

/* 每张显卡独立页面 */
typedef struct {
    GtkWidget *title;
    GtkWidget *sub;
    GtkWidget *area;
    GtkWidget *temp_area;
    GtkWidget *mem_area;
    GtkWidget *small;
    GtkWidget *grid;
    CurveData  curve;
    CurveData  temp_curve;
    CurveData  mem_curve;
    double     temp_hist[FMON_HISTORY_POINTS];
    uint64_t   temp_time[FMON_HISTORY_POINTS];
    double     mem_hist[FMON_HISTORY_POINTS];
    uint64_t   mem_time[FMON_HISTORY_POINTS];
} GpuPage;

static GpuPage g_gpu_pages[FMON_MAX_GPUS];

/* 曲线悬停：记录鼠标位置，交给 gui_chart 画出竖线与读数 */
static void gpu_chart_motion(GtkEventControllerMotion *ctrl, double x, double y,
                             gpointer data)
{
    (void)y;

    CurveData *cd = data;

    cd->hovering = TRUE;
    cd->hover_x  = x;

    gtk_widget_queue_draw(gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(ctrl)));
}

static void gpu_chart_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    CurveData *cd = data;

    cd->hovering = FALSE;

    gtk_widget_queue_draw(gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(ctrl)));
}

static const FmonShmData *snap(void)
{
    return gui_state_snapshot();
}

static void gpu_push_history(CurveData *cd, double value)
{
    if (cd == NULL || cd->history == NULL || !isfinite(value) || value < 0.0)
        return;

    gint64 now = g_get_monotonic_time();

    ((double *)cd->history)[cd->index] = value;
    if (cd->time_us != NULL)
        ((uint64_t *)cd->time_us)[cd->index] = (uint64_t)now;

    cd->index = (cd->index + 1) % FMON_HISTORY_POINTS;
    if (cd->count < FMON_HISTORY_POINTS)
        cd->count++;
    cd->end_time_us = (uint64_t)now;
    cd->interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
}

static GtkWidget *gpu_small_chart(CurveData *cd, const char *title,
                                  GtkWidget **out_area)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);
    GtkWidget *l = gtk_label_new(title);

    gtk_widget_add_css_class(l, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_box_append(GTK_BOX(box), l);

    GtkWidget *area = gtk_drawing_area_new();

    gtk_widget_set_size_request(area, -1, 150);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), gui_chart_draw, cd,
                                   NULL);
    gui_chart_attach_interactions(area, cd);
    gtk_box_append(GTK_BOX(box), area);

    *out_area = area;
    return box;
}

GtkWidget *gui_gpu_page_new(int slot)
{
    GpuPage *pg = &g_gpu_pages[slot];

    memset(pg, 0, sizeof(*pg));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_2);
    gtk_widget_set_margin_top(vbox, FMON_SPACE_4);
    gtk_widget_set_margin_bottom(vbox, FMON_SPACE_4);
    gtk_widget_set_margin_start(vbox, FMON_SPACE_3);
    gtk_widget_set_margin_end(vbox, FMON_SPACE_3);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);
    gtk_widget_add_css_class(card, "page-card");

    char title[32];

    g_snprintf(title, sizeof(title), "GPU %d", slot);

    pg->title = gtk_label_new(title);
    gtk_widget_add_css_class(pg->title, "group-title");
    gtk_label_set_xalign(GTK_LABEL(pg->title), 0.0f);

    pg->sub = gtk_label_new("—");
    gtk_widget_add_css_class(pg->sub, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(pg->sub), 0.0f);

    pg->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(pg->area, -1, 320);
    gtk_widget_set_hexpand(pg->area, TRUE);
    gtk_widget_set_vexpand(pg->area, TRUE);

    pg->curve.history     = snap()->gpu_history[slot];
    pg->curve.max_val     = 100.0;
    pg->curve.unit        = "%";
    pg->curve.time_us     = snap()->history_time_us;
    pg->curve.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    /* 每张卡一个固定颜色，避免曲线默认黑色 */
    /* GPU 统一使用黄色语义；多卡用明度区分，不改变“GPU=黄”的约定。 */
    static const double colors[FMON_MAX_GPUS][3] = {
        { 1.00, 1.00, 0.00 },
        { 0.80, 0.80, 0.00 },
        { 1.00, 0.80, 0.00 },
        { 0.80, 1.00, 0.00 },
    };

    pg->curve.r = colors[slot % FMON_MAX_GPUS][0];
    pg->curve.g = colors[slot % FMON_MAX_GPUS][1];
    pg->curve.b = colors[slot % FMON_MAX_GPUS][2];

    pg->temp_curve.history = pg->temp_hist;
    pg->temp_curve.time_us = pg->temp_time;
    pg->temp_curve.unit = "°C";
    pg->temp_curve.max_val = 100.0;
    pg->temp_curve.r = 1.00;
    pg->temp_curve.g = 0.00;
    pg->temp_curve.b = 0.00;
    pg->temp_curve.empty_hint = "长历史模式显示 GPU 温度";
    pg->mem_curve.history = pg->mem_hist;
    pg->mem_curve.time_us = pg->mem_time;
    pg->mem_curve.unit = "MB";
    pg->mem_curve.max_val = 0.0;
    pg->mem_curve.r = 1.00;
    pg->mem_curve.g = 1.00;
    pg->mem_curve.b = 0.00;
    pg->mem_curve.empty_hint = "长历史模式显示显存使用";

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pg->area), gui_chart_draw,
                                   &pg->curve, NULL);
    gui_chart_attach_interactions(pg->area, &pg->curve);

    /* 独立显卡页同样需要悬停读数 */
    GtkEventController *motion = gtk_event_controller_motion_new();

    g_signal_connect(motion, "motion", G_CALLBACK(gpu_chart_motion),
                     &pg->curve);
    g_signal_connect(motion, "leave", G_CALLBACK(gpu_chart_leave), &pg->curve);
    gtk_widget_add_controller(pg->area, motion);

    gtk_box_append(GTK_BOX(card), pg->title);
    gtk_box_append(GTK_BOX(card), pg->sub);
    gtk_box_append(GTK_BOX(card), pg->area);

    GtkWidget *small = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_3);
    GtkWidget *temp_area = NULL;
    GtkWidget *mem_area = NULL;

    gtk_box_append(GTK_BOX(small),
                   gpu_small_chart(&pg->temp_curve, "GPU 温度", &temp_area));
    gtk_box_append(GTK_BOX(small),
                   gpu_small_chart(&pg->mem_curve, "显存使用", &mem_area));
    gtk_box_append(GTK_BOX(card), small);

    pg->temp_area = temp_area;
    pg->mem_area = mem_area;
    pg->small = small;
    gtk_widget_set_visible(small, TRUE);

    /* 详情并入同一张大卡片（与 CPU 页一致），不再单独占一张卡 */
    GtkWidget *dt = gtk_label_new(_("显卡详情"));

    gtk_widget_add_css_class(dt, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(dt), 0.0f);
    gtk_widget_set_margin_top(dt, FMON_SPACE_3);

    pg->grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(pg->grid), FMON_SPACE_1);
    gtk_grid_set_column_spacing(GTK_GRID(pg->grid), FMON_SPACE_5);
    gtk_widget_set_margin_top(pg->grid, FMON_SPACE_1);

    gtk_box_append(GTK_BOX(card), dt);
    gtk_box_append(GTK_BOX(card), pg->grid);

    gtk_box_append(GTK_BOX(vbox), card);

    return vbox;
}

CurveData *gui_gpu_curve(int slot)
{
    if (slot < 0 || slot >= FMON_MAX_GPUS)
        return NULL;

    return &g_gpu_pages[slot].curve;
}

CurveData *gui_gpu_temp_curve(int slot)
{
    if (slot < 0 || slot >= FMON_MAX_GPUS)
        return NULL;

    return &g_gpu_pages[slot].temp_curve;
}

CurveData *gui_gpu_mem_curve(int slot)
{
    if (slot < 0 || slot >= FMON_MAX_GPUS)
        return NULL;

    return &g_gpu_pages[slot].mem_curve;
}

void gui_gpu_pages_update(void)
{
    for (int slot = 0; slot < FMON_MAX_GPUS; slot++) {
        GpuPage *pg = &g_gpu_pages[slot];

        if (pg->area == NULL)
            continue;

        if (!gtk_widget_get_mapped(pg->area))
            continue;   /* 不可见的 GPU 页不更新标签 */

        if (slot >= (int)snap()->gpu_count) {
            char t[32];

            g_snprintf(t, sizeof(t), "GPU %d", slot + 1);
            gtk_label_set_text(GTK_LABEL(pg->title), t);
            gtk_label_set_text(GTK_LABEL(pg->sub), "未检测到该显卡");
            pg->curve.count = 0;
            pg->curve.empty_hint = "未检测到该显卡";
            if (pg->small != NULL)
                gtk_widget_set_visible(pg->small, FALSE);
            gtk_widget_queue_draw(pg->area);
            continue;
        }

        const FmonGpuInfo *g = &snap()->gpus[slot];
        double pc[4];
        double shade = 1.0 - 0.12 * (double)(slot % 4);

        fmon_palette_get(FMON_COLOR_GPU, pc);
        pg->curve.r = pc[0] * shade;
        pg->curve.g = pc[1] * shade;
        pg->curve.b = pc[2] * shade;
        fmon_palette_get(FMON_COLOR_TEMP, pc);
        pg->temp_curve.r = pc[0];
        pg->temp_curve.g = pc[1];
        pg->temp_curve.b = pc[2];
        fmon_palette_get(FMON_COLOR_DISK_READ, pc);
        pg->mem_curve.r = pc[0];
        pg->mem_curve.g = pc[1];
        pg->mem_curve.b = pc[2];

        gtk_label_set_text(GTK_LABEL(pg->title), g->name);

        char sub[192];

        g_snprintf(sub, sizeof(sub), "%s · %s · %s",
                   (g->vendor[0] != '\0') ? g->vendor : "GPU",
                   (g->kind == FMON_GPU_INTEGRATED) ? "核显" : "独显",
                   (g->load_available != 0) ? "传感器可用" : "无传感器");
        gtk_label_set_text(GTK_LABEL(pg->sub), sub);

        if (!pg->curve.frozen) {
            pg->curve.time_us = snap()->history_time_us;
            pg->curve.count = snap()->history_count;
            pg->curve.index = snap()->history_index;
            pg->curve.empty_hint = (g->load_available != 0)
                                       ? "正在采集…"
                                       : "该显卡没有可用的负载传感器";
        }

        if (g->load_available != 0) {
            if (!pg->temp_curve.frozen)
                gpu_push_history(&pg->temp_curve, g->temp_c);
            if (!pg->mem_curve.frozen)
                gpu_push_history(&pg->mem_curve, g->mem_used_mb);
        }

        if (pg->small != NULL)
            gtk_widget_set_visible(pg->small, TRUE);

        gtk_widget_queue_draw(pg->area);
        if (pg->temp_area != NULL)
            gtk_widget_queue_draw(pg->temp_area);
        if (pg->mem_area != NULL)
            gtk_widget_queue_draw(pg->mem_area);
        gui_gpu_fill_details(pg->grid, slot);
    }
}
