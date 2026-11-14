/*
 * gui_chart.c - 绘制平滑曲线
 *
 * 设计要点：
 *   1. 只画真实采集到的点。旧版本会把尚未采满的环形缓冲区（内容为 0）
 *      也画出来，于是曲线前 2 分钟一直贴着 0 走，看起来像“断崖”。
 *   2. 删除旧版的“突变过滤”：那段代码会把真实尖峰削平、把突增按比例
 *      缩小，属于对监控数据的篡改。降噪应该发生在采集侧（EMA），
 *      而不是在绘图时改数据。
 *   3. 贝塞尔控制点被限制在相邻两点的高度区间内，避免三次曲线过冲
 *      画出“根本不存在的峰值”。
 */

#include "gui_chart.h"
#include "gui_chart_util.h"

#include "gui_util.h"

#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* 相对时间标签：-5m / -30s / 现在 */
void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data);

void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data)
{
    (void)area;

    if (getenv("FMON_NO_CHART_DRAW") != NULL)
        return;

    CurveData *cd = data;
    if (cd == NULL || cd->history == NULL)
        return;


    const int margin_left   = 46;
    const int margin_right  = 8;
    const int margin_top    = 8;
    const int margin_bottom = 18;

    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    if (plot_w < 20 || plot_h < 20)
        return;

    uint32_t points = cd->count;
    if (points > FMON_HISTORY_POINTS)
        points = FMON_HISTORY_POINTS;

    /* Y 轴量程 */
    double effective_max = cd->max_val;
    if (effective_max <= 0.0) {
        double peak = 1.0;
        for (uint32_t i = 0; i < points; i++) {
            uint32_t idx = (cd->index + FMON_HISTORY_POINTS - points + i) %
                           FMON_HISTORY_POINTS;
            double v = cd->history[idx];
            if (v > peak)
                peak = v;
        }
        effective_max = gui_chart_nice_max(peak);
    }
    if (!(effective_max > 0.0))
        effective_max = 1.0;

    /* 命中率等长期接近满值的指标：下限跟随数据，否则 0-100 曲线贴顶不动 */
    double effective_min = cd->min_val;

    if (effective_min < 0.0) {
        double lo = 1e18;

        for (uint32_t i = 0; i < points; i++) {
            uint32_t idx = (cd->index + FMON_HISTORY_POINTS - points + i) %
                           FMON_HISTORY_POINTS;
            double v = cd->history[idx];

            if (isfinite(v) && v < lo)
                lo = v;
        }

        if (!(lo < 1e18))
            lo = 0.0;

        effective_min = floor(lo / 5.0) * 5.0;

        if (effective_min < 0.0)
            effective_min = 0.0;
        if (effective_min > effective_max - 5.0)
            effective_min = effective_max - 5.0;
        if (effective_min < 0.0)
            effective_min = 0.0;
    }

    if (effective_max <= effective_min)
        effective_max = effective_min + 1.0;

    /* 曲线区域独立面板背景（与窗口背景区分） */
    fmon_gui_draw_panel(cr, margin_left - 6.0, margin_top - 6.0,
                        (double)plot_w + 14.0, (double)plot_h + 10.0,
                        0.60, 0.60, 0.65, 0.10);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    double axis_col[4];

    fmon_theme_axis_color(axis_col);

    cairo_font_face_t *font =
        cairo_toy_font_face_create("Sans", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_face(cr, font);
    cairo_font_face_destroy(font);
    cairo_set_font_size(cr, 11.0);

    /* 网格 + 刻度 */
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 4; i++) {
        double y_val = effective_max -
                       (effective_max - effective_min) * (double)i / 4.0;
        double y_pos = margin_top + (double)plot_h * (double)i / 4.0;

        double grid[4];

        fmon_theme_grid_color(grid, FALSE);
        cairo_set_source_rgba(cr, grid[0], grid[1], grid[2], grid[3]);
        cairo_move_to(cr, margin_left, y_pos);
        cairo_line_to(cr, width - margin_right, y_pos);
        cairo_stroke(cr);

        char label[32];
        gui_chart_format_axis_label(label, sizeof(label), y_val, cd->unit,
                                    effective_max - effective_min);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);
        cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2], 1.0);
        cairo_move_to(cr, margin_left - ext.width - 6.0,
                      y_pos + ext.height / 2.0);
        cairo_show_text(cr, label);
    }

    /* X 轴时间刻度：相对当前时刻 */
    if (points >= 2 && plot_w >= 220) {
        double total_sec = (double)(points - 1) *
                           (double)cd->interval_us / 1000000.0;

        for (int t = 0; t <= 3; t++) {
            double frac = (double)t / 3.0;
            double secs = total_sec * (1.0 - frac);

            char tbuf[24];
            gui_chart_format_time_label(tbuf, sizeof(tbuf), secs);

            cairo_text_extents_t ext;
            cairo_text_extents(cr, tbuf, &ext);

            double tx = margin_left + frac * (double)plot_w - ext.width / 2.0;
            if (t == 0)
                tx = margin_left;
            if (t == 3)
                tx = margin_left + plot_w - ext.width;

            cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2], 0.75);
            cairo_move_to(cr, tx, margin_top + plot_h + ext.height + 4.0);
            cairo_show_text(cr, tbuf);
        }
    }

    if (points < 2) {
        const char *hint = (cd->empty_hint != NULL) ? cd->empty_hint
                                                   : "正在采集…";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, hint, &ext);
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.8);
        cairo_move_to(cr, margin_left + (plot_w - ext.width) / 2.0,
                      margin_top + (plot_h + ext.height) / 2.0);
        cairo_show_text(cr, hint);
        cairo_stroke(cr);
        return;
    }

    static double px[FMON_HISTORY_POINTS];
    static double py[FMON_HISTORY_POINTS];


    double x_step = (double)plot_w / (double)(points - 1);
    uint32_t start = (cd->index + FMON_HISTORY_POINTS - points) %
                     FMON_HISTORY_POINTS;

    double   peak_v = -1.0;
    uint32_t peak_i = 0;

    for (uint32_t i = 0; i < points; i++) {
        uint32_t idx = (start + i) % FMON_HISTORY_POINTS;
        double v = cd->history[idx];

        /* 仅做显示范围裁剪，不修改数据本身 */
        if (v > effective_max)
            v = effective_max;
        if (v < effective_min)
            v = effective_min;
        if (v < 0.0 || !isfinite(v))
            v = 0.0;

        if (v > peak_v) {
            peak_v = v;
            peak_i = i;
        }

        px[i] = margin_left + (double)i * x_step;
        py[i] = margin_top + (double)plot_h -
                ((v - effective_min) / (effective_max - effective_min)) *
                    (double)plot_h;
    }

    /* 阶梯（step）路径：先水平保持，再垂直跳变。
     * 系统指标在两次采样之间是保持值，不能用斜线制造中间态。 */
    #define CURVE_SEGMENTS()                                        \
        do {                                                        \
            for (uint32_t i = 0; i + 1 < points; i++) {            \
                cairo_line_to(cr, px[i + 1], py[i]);               \
                cairo_line_to(cr, px[i + 1], py[i + 1]);           \
            }                                                       \
        } while (0)

    /* 1. 渐变填充 */
    cairo_pattern_t *pat = cairo_pattern_create_linear(
        0.0, margin_top, 0.0, margin_top + plot_h);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, cd->r, cd->g, cd->b, 0.28);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, cd->r, cd->g, cd->b, 0.02);

    cairo_move_to(cr, px[0], margin_top + plot_h);
    cairo_line_to(cr, px[0], py[0]);
    CURVE_SEGMENTS();
    cairo_line_to(cr, px[points - 1], margin_top + plot_h);
    cairo_close_path(cr);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    /* 2. 曲线 */
    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_move_to(cr, px[0], py[0]);
    CURVE_SEGMENTS();
    cairo_stroke(cr);

    #undef CURVE_SEGMENTS

    /* 3. 末端高亮圆点 */
    cairo_arc(cr, px[points - 1], py[points - 1], 2.2, 0.0, 2.0 * M_PI);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_fill(cr);

    /* 4. 悬停读数：竖线 + 数据点 + 数值/时刻 */
    if (cd->hovering && cd->hover_x >= margin_left &&
        cd->hover_x <= margin_left + plot_w) {

        double rel = (cd->hover_x - margin_left) / x_step;
        if (rel < 0.0)
            rel = 0.0;
        if (rel > (double)(points - 1))
            rel = (double)(points - 1);

        uint32_t i  = (uint32_t)(rel + 0.5);
        double   hx = px[i];
        double   hy = py[i];

        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.45);
        cairo_move_to(cr, hx, margin_top);
        cairo_line_to(cr, hx, margin_top + plot_h);
        cairo_stroke(cr);

        cairo_arc(cr, hx, hy, 3.4, 0.0, 2.0 * M_PI);
        cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
        cairo_fill(cr);
        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_arc(cr, hx, hy, 3.4, 0.0, 2.0 * M_PI);
        cairo_stroke(cr);

        /* 采样时刻：相对最新点，精确到毫秒 */
        uint32_t idx   = (start + i) % FMON_HISTORY_POINTS;
        double   value = cd->history[idx];
        double   secs  = (double)(points - 1 - i) *
                         (double)cd->interval_us / 1000000.0;

        char label[96];

        if (cd->unit != NULL && strcmp(cd->unit, "%") == 0)
            snprintf(label, sizeof(label), "%.2f%%  -%.3fs", value, secs);
        else if (cd->unit != NULL && cd->unit[0] != '\0')
            snprintf(label, sizeof(label), "%.2f %s  -%.3fs", value,
                     cd->unit, secs);
        else
            snprintf(label, sizeof(label), "%.2f  -%.3fs", value, secs);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);

        double box_w = ext.width + 14.0;
        double box_h = ext.height + 8.0;
        double box_x = hx + 8.0;
        double box_y = hy - box_h - 8.0;

        if (box_x + box_w > width - margin_right)
            box_x = hx - box_w - 8.0;
        if (box_y < margin_top)
            box_y = hy + 8.0;

        double hb[4];

        fmon_theme_hover_bg(hb);
        cairo_set_source_rgba(cr, hb[0], hb[1], hb[2], 0.92);
        cairo_rectangle(cr, box_x, box_y, box_w, box_h);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.8);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, box_x, box_y, box_w, box_h);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 1.0);
        cairo_move_to(cr, box_x + 7.0, box_y + 4.0 + ext.height);
        cairo_show_text(cr, label);

        /* 峰值参考线：悬停时标出当前窗口的最大值 */
        if (peak_v >= 0.0 && peak_i < points) {
            double py_peak = py[peak_i];
            double dashes[] = { 4.0, 4.0 };

            cairo_save(cr);
            cairo_set_dash(cr, dashes, 2, 0.0);
            cairo_set_line_width(cr, 1.0);
            cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.55);
            cairo_move_to(cr, margin_left, py_peak);
            cairo_line_to(cr, margin_left + plot_w, py_peak);
            cairo_stroke(cr);
            cairo_restore(cr);

            char pbuf[64];
            if (cd->unit != NULL && cd->unit[0] != '\0')
                snprintf(pbuf, sizeof(pbuf), "峰值 %.2f%s", peak_v, cd->unit);
            else
                snprintf(pbuf, sizeof(pbuf), "峰值 %.2f", peak_v);

            cairo_text_extents_t pext;
            cairo_text_extents(cr, pbuf, &pext);

            /* 水平：右对齐到绘图区，但不能超出左边界 */
            double lx = margin_left + plot_w - pext.width - 6.0;
            if (lx < margin_left + 2.0)
                lx = margin_left + 2.0;

            /* 垂直：优先放在峰值线上方；顶部放不下就改到下方，
             * 这样峰值等于量程上限时文字不会被裁掉 */
            double ly = py_peak - 5.0;
            if (ly - pext.height < margin_top + 1.0)
                ly = py_peak + pext.height + 6.0;
            if (ly > margin_top + plot_h - 2.0)
                ly = margin_top + plot_h - 2.0;

            /* 底色：避免压在网格/曲线上看不清 */
            double pb[4];

            fmon_theme_hover_bg(pb);
            cairo_set_source_rgba(cr, pb[0], pb[1], pb[2], 0.90);
            cairo_rectangle(cr, lx - 4.0, ly - pext.height - 2.0,
                            pext.width + 8.0, pext.height + 6.0);
            cairo_fill(cr);

            cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 1.0);
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, pbuf);
        }
    }
}




