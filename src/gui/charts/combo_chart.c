/*
 * combo_chart.c - 多曲线合并图表实现
 *
 * 结构说明（每个关注点独立成一个静态函数，便于单测/复用）：
 *   - 几何：combo_geom_init() / band_layout() / band_y()
 *   - 背景：draw_grid() / draw_axis_labels()
 *   - 单条带：draw_band_frame() / draw_band_label()
 *   - 曲线：draw_series_fill() / draw_series_line()
 *   - 交互：draw_hover_line() / draw_hover_dots() / draw_hover_box()
 *
 * 每个条带左侧绘制 0/中/最大刻度，尖峰绝对值不用悬停也能判断。
 */

#include "combo_chart.h"
#include "combo_chart_internal.h"

#include "gui_util.h"

#include "theme.h"

#include <math.h>
#include <string.h>

#define COMBO_MAX_POINTS  FMON_HISTORY_POINTS
#define COMBO_TAU         6.28318530717958647692

/* -------------------------------------------------------------------------
 * 数据访问
 * ------------------------------------------------------------------------- */



/* -------------------------------------------------------------------------
 * 数值文本：带名、悬停读数共用，保证表达完全一致
 * ------------------------------------------------------------------------- */

/* 浅色主题下把系列色压暗，保证文字在浅底上可读 */


/* -------------------------------------------------------------------------
 * 几何
 * ------------------------------------------------------------------------- */




/* -------------------------------------------------------------------------
 * 字体与时间文本
 * ------------------------------------------------------------------------- */



/* -------------------------------------------------------------------------
 * 背景网格与时间轴
 * ------------------------------------------------------------------------- */



/* -------------------------------------------------------------------------
 * 单条带：边框 / 名称 + 当前值
 * ------------------------------------------------------------------------- */



/* -------------------------------------------------------------------------
 * 曲线：渐变填充 + 本体
 * ------------------------------------------------------------------------- */



/* -------------------------------------------------------------------------
 * 悬停交互
 * ------------------------------------------------------------------------- */





/* -------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */

/* 每个条带左侧的 0/中/最大值刻度：悬停之前就能判断尖峰绝对值 */
/* 纵轴刻度：范围很窄时提高小数位，避免 41.7/41.8 显示成同一个数 */
static void format_axis_tick(char *buf, size_t n, double v, double span,
                             const char *unit)
{
    if (unit != NULL && strcmp(unit, "KB/s") == 0) {
        format_value_text(buf, n, v, unit);
        return;
    }

    int dec = 1;

    if (span < 0.05)
        dec = 3;
    else if (span < 0.5)
        dec = 2;
    else if (span < 5.0)
        dec = 2;
    else if (span < 50.0)
        dec = 1;
    else
        dec = 0;

    const char *suffix = (unit != NULL) ? unit : "";

    g_snprintf(buf, n, "%.*f%s", dec, v, suffix);
}

static void draw_band_ticks(cairo_t *cr, const ComboGeom *g,
                            const BandGeom *b, double lo, double hi,
                            const char *unit)
{
    double axis[4];
    double span = hi - lo;

    fmon_theme_axis_color(axis);
    set_font(cr, 9.5);

    for (int i = 0; i < 3; i++) {
        double v = lo + (hi - lo) * ((double)i / 2.0);
        char   txt[48];

        format_axis_tick(txt, sizeof(txt), v, span, unit);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, txt, &ext);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.85);
        double tx = g->left - 8.0 - ext.width;

        if (tx < 2.0)
            tx = 2.0;

        cairo_move_to(cr, tx, band_y(b, v, lo, hi) + 3.5);
        cairo_show_text(cr, txt);
    }
}

void combo_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer data)
{
    (void)area;

    if (getenv("FMON_NO_CHART_DRAW") != NULL)
        return;

    ComboChart *c = data;
    ComboGeom   g;

    combo_geom_init(&g, width, height);

    if (g.plot_w < 20.0 || g.plot_h < 20.0 || c->n == 0)
        return;

    draw_grid(cr, &g, c->n);
    draw_axis_labels(cr, &g, c);

    for (int s = 0; s < c->n; s++) {
        const CurveData *cd = c->series[s].cd;
        BandGeom         b;
        double           lo;
        double           hi;

        band_layout(&g, c->n, s, &b);
        draw_band_frame(cr, &g, &b, cd);
        draw_band_label(cr, &g, &b, &c->series[s], cd);

        if (cd->count < 2)
            continue;

        series_range(cd, &lo, &hi);

        /* 温度这类数值范围很窄：先在 0–100 的固定范围里画，避免一条
         * 42.8/42.9 的线被放大成锯齿；缺少 SMART 时没有数据点，自然空着。 */
        if (cd->unit != NULL && strcmp(cd->unit, "°C") == 0) {
            lo = 0.0;
            hi = 100.0;
        }

        draw_band_ticks(cr, &g, &b, lo, hi, cd->unit);
        draw_series_fill(cr, &g, &b, cd, lo, hi);
        draw_series_line(cr, &g, &b, cd, lo, hi);
    }

    draw_hover(cr, &g, c);
}

void combo_chart_motion(GtkEventControllerMotion *ctrl, double x, double y,
                        gpointer data)
{
    ComboChart *c = data;
    GtkWidget  *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));

    c->hovering = TRUE;
    c->hover_x  = x;
    c->hover_y  = y;
    gtk_widget_queue_draw(w);
}

void combo_chart_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    ComboChart *c = data;
    GtkWidget  *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));

    c->hovering = FALSE;
    gtk_widget_queue_draw(w);
}


