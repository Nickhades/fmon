/*
 * gui_coreview.c - CPU 每线程使用率（单画布绘制，字号绝对统一）
 */

#include "gui_coreview.h"

#include "gui_state.h"
#include "theme.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <sys/sysctl.h>

#define ROW_H        20
#define COL_W_MIN    214
#define CORE_COLUMNS_MAX 4
#define BAR_W        4
#define FONT_PT      10

static const double k_colors[16][3] = {
    { 0.00, 1.00, 0.00 }, { 0.00, 1.00, 1.00 },
    { 1.00, 0.00, 1.00 }, { 1.00, 1.00, 0.00 },
    { 1.00, 0.00, 0.00 }, { 0.00, 0.70, 1.00 },
    { 1.00, 0.55, 0.00 }, { 0.55, 1.00, 0.00 },
    { 0.00, 1.00, 0.55 }, { 0.55, 0.55, 1.00 },
    { 1.00, 0.55, 1.00 }, { 1.00, 1.00, 0.55 },
    { 0.55, 1.00, 1.00 }, { 1.00, 1.00, 1.00 },
    { 0.70, 0.70, 0.70 }, { 0.40, 0.80, 1.00 },
};

static int    s_count;
static double s_pct[FMON_MAX_CPUS];
static int    s_last_rows = -1;

static int core_columns_for_width(int width)
{
    if (width <= 0)
        return CORE_COLUMNS_MAX;

    int cols = width / COL_W_MIN;

    if (cols < 1)
        cols = 1;
    if (cols > CORE_COLUMNS_MAX)
        cols = CORE_COLUMNS_MAX;

    return cols;
}

static void update_core_height(GtkWidget *area)
{
    if (area == NULL || s_count <= 0)
        return;

    int width = gtk_widget_get_width(area);
    int cols = core_columns_for_width(width);
    int rows = (s_count + cols - 1) / cols;

    if (rows < 1)
        rows = 1;

    if (rows != s_last_rows) {
        s_last_rows = rows;
        gtk_widget_set_size_request(area, -1, rows * ROW_H);
    }
}

static void on_core_width_changed(GObject *object, GParamSpec *pspec,
                                  gpointer data)
{
    (void)object;
    (void)pspec;
    (void)data;

    update_core_height(GTK_WIDGET(object));
}

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
        pango_font_description_set_family(fd, "Monospace");
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

static int text_width(PangoLayout *layout, const char *text)
{
    pango_layout_set_text(layout, text, -1);

    int w = 0, h = 0;

    pango_layout_get_pixel_size(layout, &w, &h);
    return w;
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

    if (width <= 0)
        return;

    int cols = core_columns_for_width(width);
    int colw = width / cols;

    if (colw < 1)
        return;

    gboolean dark = fmon_theme_is_dark();

    for (int i = 0; i < s_count; i++) {
        int col = i % cols;
        int row = i / cols;
        double x = col * colw;
        double y = row * ROW_H;
        int idx = i % 16;
        double shade = dark ? 1.0 : 0.55;

        cairo_set_source_rgb(cr, k_colors[idx][0] * shade,
                             k_colors[idx][1] * shade,
                             k_colors[idx][2] * shade);
        cairo_rectangle(cr, x, y + (ROW_H - 11) / 2.0, BAR_W, 11);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.95);

        gchar *name = g_strdup_printf(_("CPU %d 总使用率"), i + 1);
        int name_w = text_width(layout, name);

        draw_text(cr, layout, name, x + BAR_W + 7, y, FALSE);
        g_free(name);

        gchar *val = g_strdup_printf("%.1f %%", s_pct[i]);

        /* 数值紧跟名称，避免被误读成下一列的读数。 */
        draw_text(cr, layout, val, x + BAR_W + 7 + name_w + 8, y, FALSE);
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
    g_signal_connect(area, "notify::width",
                     G_CALLBACK(on_core_width_changed), NULL);
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

    update_core_height(area);
    gtk_widget_queue_draw(area);
}
