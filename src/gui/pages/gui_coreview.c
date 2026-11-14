/*
 * gui_coreview.c - CPU 每线程使用率（单画布绘制，字号绝对统一）
 */

#include "gui_coreview.h"

#include "gui_state.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <sys/sysctl.h>

#define CORE_COLUMNS 4
#define ROW_H        20
#define COL_W        214
#define BAR_W        4
#define FONT_PT      10

static const double k_colors[16][3] = {
    { 0.20, 0.55, 0.95 }, { 0.90, 0.30, 0.85 },
    { 0.95, 0.45, 0.55 }, { 0.90, 0.25, 0.25 },
    { 0.95, 0.60, 0.20 }, { 0.85, 0.85, 0.20 },
    { 0.40, 0.80, 0.30 }, { 0.20, 0.80, 0.60 },
    { 0.25, 0.85, 0.85 }, { 0.35, 0.65, 0.95 },
    { 0.70, 0.45, 0.95 }, { 0.95, 0.40, 0.75 },
    { 0.60, 0.85, 0.30 }, { 0.95, 0.75, 0.30 },
    { 0.40, 0.90, 0.70 }, { 0.30, 0.70, 0.95 },
};

static int    s_count;
static double s_pct[FMON_MAX_CPUS];

/* 物理核数：kern.smp.cores；取不到时返回 0 */
static int physical_cores(void)
{
    static int cached = -1;

    if (cached < 0) {
        int    v = 0;
        size_t l = sizeof(v);

        cached = (sysctlbyname("kern.smp.cores", &v, &l, NULL, 0) == 0 &&
                  v > 0)
                     ? v
                     : 0;
    }

    return cached;
}

static PangoFontDescription *core_font(void)
{
    static PangoFontDescription *fd;

    if (fd == NULL) {
        fd = pango_font_description_new();
        pango_font_description_set_family(fd, "Sans");
        pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
        pango_font_description_set_size(fd, FONT_PT * PANGO_SCALE);
    }

    return fd;
}

static void draw_text(cairo_t *cr, PangoLayout *layout, const char *text,
                      double x, double y, gboolean right_align)
{
    pango_layout_set_text(layout, text, -1);

    int tw = 0, th = 0;

    pango_layout_get_pixel_size(layout, &tw, &th);

    if (right_align)
        x -= tw;

    cairo_move_to(cr, x, y + (ROW_H - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
}

static void draw_list(GtkDrawingArea *area, cairo_t *cr, int width,
                      int height, gpointer data)
{
    (void)height;

    if (s_count <= 0)
        return;

    GdkRGBA fg;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.95);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    pango_layout_set_font_description(layout, core_font());

    (void)width;

    const int name_w = 110;   /* 「CPU N 总使用率」固定宽度 */
    const int colw   = COL_W;

    for (int i = 0; i < s_count; i++) {
        int col = i % CORE_COLUMNS;
        int row = i / CORE_COLUMNS;
        double x = col * colw;
        double y = row * ROW_H;
        int idx = i % 16;

        cairo_set_source_rgb(cr, k_colors[idx][0], k_colors[idx][1],
                             k_colors[idx][2]);
        cairo_rectangle(cr, x, y + (ROW_H - 11) / 2.0, BAR_W, 11);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.95);

        gchar *name = g_strdup_printf(_("CPU %d 总使用率"), i + 1);

        draw_text(cr, layout, name, x + BAR_W + 7, y, FALSE);
        g_free(name);

        gchar *val = g_strdup_printf("%.1f %%", s_pct[i]);

        /* 数值紧跟名称，固定起点，不再右对齐到列尾 */
        draw_text(cr, layout, val, x + BAR_W + 7 + name_w + 6, y, FALSE);
        g_free(val);
    }

    g_object_unref(layout);
}

GtkWidget *gui_core_view_new(void)
{
    GtkWidget *area = gtk_drawing_area_new();

    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_size_request(area, -1, ROW_H * 2);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_list, NULL,
                                   NULL);
    return area;
}

void gui_core_view_update(GtkWidget *area, GtkWidget *caption)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (caption != NULL) {
        int cores = physical_cores();

        /* 与曲线带内显示保持同一数据源：取 CPU 历史曲线的最后一个点 */
        double total = snap->cpu_usage_pct;

        if (snap->history_count > 0) {
            guint last = (snap->history_index + FMON_HISTORY_POINTS - 1) %
                         FMON_HISTORY_POINTS;

            total = snap->cpu_history[last];
        }

        gchar *txt;

        if (cores > 0)
            txt = g_strdup_printf(
                _("%d 核 · %u 线程 · 总使用率 %.1f %%"), cores,
                (unsigned)snap->cpu_count, total);
        else
            txt = g_strdup_printf(_("%u 线程 · 总使用率 %.1f %%"),
                                  (unsigned)snap->cpu_count, total);

        gtk_label_set_text(GTK_LABEL(caption), txt);
        g_free(txt);
    }

    if (area == NULL)
        return;

    s_count = MIN((int)snap->cpu_count, FMON_MAX_CPUS);

    for (int i = 0; i < FMON_MAX_CPUS; i++)
        s_pct[i] = snap->cpu_core_pct[i];

    static int last_rows = -1;

    int rows = (s_count + CORE_COLUMNS - 1) / CORE_COLUMNS;

    if (rows < 1)
        rows = 1;

    /* 只有在行数变化时改高度，避免每次刷新触发整页重新布局 */
    if (rows != last_rows) {
        last_rows = rows;
        gtk_widget_set_size_request(area, -1, rows * ROW_H);
    }
    gtk_widget_queue_draw(area);
}








