/*
 * gui_chart.c - 单曲线折线图
 *
 * 绘图规则（与“专业监控折线”一致）：
 *   1. 采样点之间用直线连接，不做样条平滑、不做突变过滤；
 *   2. NaN/Inf 表示没有采样，折线在此断开，绝不补 0；
 *   3. 坐标轴覆盖窗口内全部有效值，固定量程只是基准，峰值超出时轴会扩展；
 *   4. 点数多于像素列时按列保留首/末/最小/最大，尖峰不会被抽掉；
 *   5. 有逐点时间戳时按真实时间映射 x，daemon 停摆留下真实时间空洞；
 *   6. 24 小时历史页可额外传入 min/max 包络，在平均线后画极值带；
 *   7. 悬停吸附到真实采样点，读数就是采样值，不做插值臆造。
 */

#include "gui_chart.h"

#include "chart_geom.h"
#include "gui_chart_util.h"
#include "gui_util.h"
#include "theme.h"

#include <glib/gstdio.h>

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static gboolean g_chart_auto_range;

static gchar *chart_settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fmon",
                            "chart_auto_range", NULL);
}

void gui_chart_settings_init(void)
{
    gchar *path = chart_settings_path();
    gchar *data = NULL;

    if (g_file_get_contents(path, &data, NULL, NULL) && data != NULL) {
        g_strstrip(data);
        g_chart_auto_range = (strcmp(data, "1") == 0);
    }

    g_free(data);
    g_free(path);
}

gboolean gui_chart_auto_range(void)
{
    return g_chart_auto_range;
}

void gui_chart_set_auto_range(gboolean enabled)
{
    g_chart_auto_range = enabled ? TRUE : FALSE;

    gchar *path = chart_settings_path();
    gchar *dir  = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);
    (void)g_file_set_contents(path, g_chart_auto_range ? "1\n" : "0\n", -1,
                              NULL);

    g_free(dir);
    g_free(path);
}

static gboolean auto_range_for(const CurveData *cd)
{
    return g_chart_auto_range && cd != NULL && cd->unit != NULL &&
           strcmp(cd->unit, "%") == 0;
}

static void set_chart_font(cairo_t *cr)
{
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
}

static void draw_hint(cairo_t *cr, int left, int top, int plot_w, int plot_h,
                      const char *text)
{
    if (text == NULL)
        return;

    cairo_text_extents_t ext;

    cairo_text_extents(cr, text, &ext);

    cairo_set_source_rgba(cr, 0.60, 0.60, 0.65, 0.85);
    cairo_move_to(cr, left + (plot_w - ext.width) / 2.0,
                  top + (plot_h + ext.height) / 2.0);
    cairo_show_text(cr, text);
}

/* 窗口内所有有效数据（平均线 + 可选 min/max 包络）的范围。 */
static bool curve_range(const CurveData *cd, uint32_t points, double *lo,
                        double *hi)
{
    bool   have = false;
    double l = 0.0, h = 0.0;

    if (fmon_chart_history_range(cd->history, FMON_HISTORY_POINTS, points,
                                 cd->index, &l, &h)) {
        *lo  = l;
        *hi  = h;
        have = true;
    }

    const double *aux[2] = { cd->history_min, cd->history_max };

    for (int i = 0; i < 2; i++) {
        if (aux[i] == NULL)
            continue;

        double al = 0.0, ah = 0.0;

        if (!fmon_chart_history_range(aux[i], FMON_HISTORY_POINTS, points,
                                      cd->index, &al, &ah))
            continue;

        if (!have) {
            *lo  = al;
            *hi  = ah;
            have = true;
        } else {
            if (al < *lo)
                *lo = al;
            if (ah > *hi)
                *hi = ah;
        }
    }

    return have;
}

static uint64_t gap_threshold_us(const CurveData *cd)
{
    if (cd->interval_us == 0)
        return 0;

    /* 2.5 个标称间隔仍未采样，就认为发生了真实时间缺口。 */
    return cd->interval_us * 5u / 2u;
}

static bool nearest_sample_by_time(const CurveData *cd, uint32_t points,
                                   uint64_t t, uint32_t *out_idx)
{
    uint64_t best = UINT64_MAX;
    uint32_t best_i = 0;

    for (uint32_t i = 0; i < points; i++) {
        uint64_t ti;
        double   v;

        if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS, points,
                                     cd->index, i, &ti))
            continue;
        if (!fmon_chart_history_value(cd->history, FMON_HISTORY_POINTS, points,
                                      cd->index, i, &v))
            continue;

        uint64_t d = (ti > t) ? ti - t : t - ti;

        if (d < best) {
            best   = d;
            best_i = i;
        }
    }

    if (best == UINT64_MAX)
        return false;

    *out_idx = best_i;
    return true;
}

/* 画一次背景/网格/坐标轴。 */
static void draw_frame(cairo_t *cr, const CurveData *cd,
                       const FmonChartScale *sc, int margin_left,
                       int margin_top, int plot_w, int plot_h)
{
    double axis_col[4];

    fmon_theme_axis_color(axis_col);

    /* 曲线区域独立面板背景（与窗口背景区分） */
    fmon_gui_draw_panel(cr, margin_left - 6.0, margin_top - 6.0,
                        (double)plot_w + 14.0, (double)plot_h + 10.0, 0.60,
                        0.60, 0.65, 0.10);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_width(cr, 1.0);

    for (int i = 0; i <= sc->intervals; i++) {
        double v   = sc->lo + sc->step * (double)i;
        double pos = (sc->hi > sc->lo)
                         ? margin_top + (double)plot_h *
                                            (sc->hi - v) / (sc->hi - sc->lo)
                         : margin_top + (double)plot_h;

        double grid[4];

        fmon_theme_grid_color(grid, FALSE);
        cairo_set_source_rgba(cr, grid[0], grid[1], grid[2], grid[3]);
        cairo_move_to(cr, margin_left, pos);
        cairo_line_to(cr, margin_left + plot_w, pos);
        cairo_stroke(cr);

        char label[48];

        gui_chart_format_axis_label(label, sizeof(label), v, cd->unit, sc->step);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, label, &ext);
        cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2], 1.0);
        cairo_move_to(cr, margin_left - ext.width - 6.0,
                      pos + ext.height / 2.0);
        cairo_show_text(cr, label);
    }
}

void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data)
{
    (void)area;

    if (getenv("FMON_NO_CHART_DRAW") != NULL)
        return;

    CurveData *cd = data;

    if (cr == NULL || cd == NULL || cd->history == NULL || width <= 0 ||
        height <= 0)
        return;

    const int margin_right  = 10;
    const int margin_top    = 8;
    const int margin_bottom = 20;

    uint32_t points = cd->count;

    if (points > FMON_HISTORY_POINTS)
        points = FMON_HISTORY_POINTS;

    double data_lo = 0.0;
    double data_hi = 0.0;
    bool   have_range = curve_range(cd, points, &data_lo, &data_hi);

    uint64_t t0 = 0, t1 = 0;
    bool     has_time = fmon_chart_time_range(
        cd->time_us, FMON_HISTORY_POINTS, points, cd->index, &t0, &t1);
    uint64_t gap_us = gap_threshold_us(cd);

    double min_span = (cd->unit != NULL && strcmp(cd->unit, "%") == 0) ? 1.0
                                                                       : 0.0;

    FmonChartScale sc;
    gboolean        auto_pct = auto_range_for(cd);

    fmon_chart_scale(have_range ? data_lo : 0.0, have_range ? data_hi : 0.0,
                     auto_pct ? -1.0
                              : ((cd->min_val >= 0.0) ? cd->min_val : -1.0),
                     auto_pct ? 0.0
                              : ((cd->max_val > 0.0) ? cd->max_val : 0.0),
                     min_span, 4, &sc);

    /* 轴标签宽度决定左边距，避免 “1.50 MB/s” 被裁掉。 */
    set_chart_font(cr);

    double max_label_w = 0.0;

    for (int i = 0; i <= sc.intervals; i++) {
        char label[48];

        gui_chart_format_axis_label(label, sizeof(label),
                                    sc.lo + sc.step * (double)i, cd->unit,
                                    sc.step);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, label, &ext);
        if (ext.width > max_label_w)
            max_label_w = ext.width;
    }

    int margin_left = (int)ceil(max_label_w) + 12;

    if (margin_left < 46)
        margin_left = 46;

    int max_left = width / 3;

    if (max_left < 32)
        max_left = 32;
    if (margin_left > max_left)
        margin_left = max_left;

    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    if (plot_w < 20 || plot_h < 20)
        return;

    draw_frame(cr, cd, &sc, margin_left, margin_top, plot_w, plot_h);

    /* X 轴时间刻度：相对当前时刻；有时间戳时使用真实时间跨度。 */
    if (points >= 2 && plot_w >= 220) {
        double total_sec;

        if (has_time)
            total_sec = (double)(t1 - t0) / 1000000.0;
        else
            total_sec = (double)(points - 1) * (double)cd->interval_us /
                        1000000.0;

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

            double axis_col[4];

            fmon_theme_axis_color(axis_col);
            cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2],
                                  0.75);
            cairo_move_to(cr, tx, margin_top + plot_h + ext.height + 4.0);
            cairo_show_text(cr, tbuf);
        }
    }

    if (points < 2) {
        draw_hint(cr, margin_left, margin_top, plot_w, plot_h,
                  (cd->empty_hint != NULL) ? cd->empty_hint : "正在采集…");
        return;
    }

    if (!have_range) {
        draw_hint(cr, margin_left, margin_top, plot_w, plot_h,
                  (cd->empty_hint != NULL) ? cd->empty_hint : "暂无有效采样");
        return;
    }

    /* 点数可能多于像素列：按列保留极值，保证尖峰不丢。 */
    size_t cap = (size_t)points + 4;
    FmonChartPoint *pts = g_new0(FmonChartPoint, cap);

    /* 有 min/max 包络时不做像素抽取：三条曲线必须使用完全相同的
     * 采样点集合，否则平均线的抽取结果与包络无法一一对应。1200 点
     * 对 Cairo 来说成本可以忽略。 */
    uint32_t max_cols = (cd->history_min != NULL && cd->history_max != NULL)
                            ? 0
                            : (uint32_t)plot_w;

    size_t n = fmon_chart_polyline_at(
        cd->history, cd->time_us, FMON_HISTORY_POINTS, points, cd->index, t0,
        t1, margin_left, (double)plot_w, margin_top, (double)plot_h, sc.lo,
        sc.hi, max_cols, gap_us, pts, cap);

    if (n == 0) {
        g_free(pts);
        draw_hint(cr, margin_left, margin_top, plot_w, plot_h,
                  (cd->empty_hint != NULL) ? cd->empty_hint : "暂无有效采样");
        return;
    }

    /* 可选 min/max 包络（仅历史页提供）。 */
    FmonChartPoint *band_lo = NULL;
    FmonChartPoint *band_hi = NULL;
    size_t          band_n  = 0;

    if (cd->history_min != NULL && cd->history_max != NULL) {
        FmonChartPoint *lo = g_new0(FmonChartPoint, cap);
        FmonChartPoint *hi = g_new0(FmonChartPoint, cap);

        size_t nlo = fmon_chart_polyline_at(
            cd->history_min, cd->time_us, FMON_HISTORY_POINTS, points,
            cd->index, t0, t1, margin_left, (double)plot_w, margin_top,
            (double)plot_h, sc.lo, sc.hi, 0, gap_us, lo, cap);
        size_t nhi = fmon_chart_polyline_at(
            cd->history_max, cd->time_us, FMON_HISTORY_POINTS, points,
            cd->index, t0, t1, margin_left, (double)plot_w, margin_top,
            (double)plot_h, sc.lo, sc.hi, 0, gap_us, hi, cap);

        if (nlo == n && nhi == n) {
            band_lo = lo;
            band_hi = hi;
            band_n  = n;
        } else {
            g_free(lo);
            g_free(hi);
        }
    }

    cairo_save(cr);
    cairo_rectangle(cr, margin_left, margin_top, plot_w, plot_h);
    cairo_clip(cr);

    if (band_n > 0) {
        /* 极值带：真实最小/最大之间的半透明区域，并描出上下边界。 */
        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.24);
        fmon_chart_cairo_band(cr, band_lo, band_hi, band_n);
        cairo_fill(cr);

        cairo_set_line_width(cr, 0.8);
        cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.38);
        fmon_chart_cairo_path(cr, band_hi, band_n);
        cairo_stroke(cr);
        fmon_chart_cairo_path(cr, band_lo, band_n);
        cairo_stroke(cr);
    } else {
        /* 1. 渐变填充（每个连续有效段单独闭合，不跨缺口） */
        cairo_pattern_t *pat = cairo_pattern_create_linear(
            0.0, margin_top, 0.0, margin_top + plot_h);

        cairo_pattern_add_color_stop_rgba(pat, 0.0, cd->r, cd->g, cd->b, 0.28);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, cd->r, cd->g, cd->b, 0.02);

        fmon_chart_cairo_fill(cr, pts, n, margin_top + plot_h);
        cairo_set_source(cr, pat);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    }

    /* 2. 折线：直线连接真实采样点 */
    cairo_set_line_width(cr, 1.35);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    fmon_chart_cairo_path(cr, pts, n);
    cairo_stroke(cr);

    /* 3. 末端高亮圆点 */
    cairo_arc(cr, pts[n - 1].x, pts[n - 1].y, 2.2, 0.0, 2.0 * M_PI);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_fill(cr);

    cairo_restore(cr);

    if (auto_pct) {
        double axis_col[4];

        fmon_theme_axis_color(axis_col);
        set_chart_font(cr);
        cairo_set_font_size(cr, 9.0);

        const char *tag = "自动量程";
        cairo_text_extents_t ext;

        cairo_text_extents(cr, tag, &ext);
        cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2],
                              0.60);
        cairo_move_to(cr, margin_left + plot_w - ext.width - 4.0,
                      margin_top + 11.0);
        cairo_show_text(cr, tag);
        set_chart_font(cr);
    }

    /* 4. 悬停读数：竖线 + 数据点 + 数值/时刻 */
    if (cd->hovering && cd->hover_x >= margin_left &&
        cd->hover_x <= margin_left + plot_w) {
        uint32_t i = 0;
        double   hx;

        if (has_time) {
            uint64_t span = t1 - t0;
            uint64_t t = t0 + (uint64_t)(((cd->hover_x - margin_left) /
                                          (double)plot_w) *
                                         (double)span);

            if (!nearest_sample_by_time(cd, points, t, &i)) {
                i = (uint32_t)(((cd->hover_x - margin_left) /
                                (double)plot_w) *
                               (double)(points - 1));
            }

            uint64_t ti;
            if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS,
                                         points, cd->index, i, &ti))
                ti = t0;
            hx = fmon_chart_time_x(ti, t0, t1, margin_left, plot_w);
        } else {
            double x_step = (double)plot_w / (double)(points - 1);
            double rel    = (cd->hover_x - margin_left) / x_step;

            if (rel < 0.0)
                rel = 0.0;
            if (rel > (double)(points - 1))
                rel = (double)(points - 1);

            i  = (uint32_t)floor(rel + 0.5);
            hx = margin_left + (double)i * x_step;
        }

        double value;
        bool   valid = fmon_chart_history_value(
            cd->history, FMON_HISTORY_POINTS, points, cd->index, i, &value);

        double secs;
        if (has_time) {
            uint64_t ti;
            if (!fmon_chart_history_time(cd->time_us, FMON_HISTORY_POINTS,
                                         points, cd->index, i, &ti))
                ti = t1;
            secs = (double)(t1 - ti) / 1000000.0;
        } else {
            secs = (double)(points - 1 - i) *
                   (double)cd->interval_us / 1000000.0;
        }

        double interval_secs = (double)cd->interval_us / 1000000.0;

        char time_txt[24];

        gui_chart_format_offset(time_txt, sizeof(time_txt), secs,
                                interval_secs);

        if (valid) {
            double hy = margin_top + (double)plot_h -
                        ((value - sc.lo) / (sc.hi - sc.lo)) * (double)plot_h;

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

            char value_txt[48];
            char label[160];

            gui_chart_format_value(value_txt, sizeof(value_txt), value,
                                   cd->unit);

            if (band_n > 0) {
                double vmin, vmax;

                if (fmon_chart_history_value(
                        cd->history_min, FMON_HISTORY_POINTS, points,
                        cd->index, i, &vmin) &&
                    fmon_chart_history_value(
                        cd->history_max, FMON_HISTORY_POINTS, points,
                        cd->index, i, &vmax)) {
                    char lo_txt[48], hi_txt[48];

                    gui_chart_format_value(lo_txt, sizeof(lo_txt), vmin,
                                           cd->unit);
                    gui_chart_format_value(hi_txt, sizeof(hi_txt), vmax,
                                           cd->unit);
                    snprintf(label, sizeof(label), "平均 %s  [%s – %s]  %s",
                             value_txt, lo_txt, hi_txt, time_txt);
                } else {
                    snprintf(label, sizeof(label), "%s  %s", value_txt,
                             time_txt);
                }
            } else {
                snprintf(label, sizeof(label), "%s  %s", value_txt, time_txt);
            }

            cairo_text_extents_t ext;

            cairo_text_extents(cr, label, &ext);

            double box_w = ext.width + 14.0;
            double box_h = ext.height + 8.0;
            double box_x = hx + 8.0;
            double box_y = hy - box_h - 8.0;

            if (box_x + box_w > width - margin_right)
                box_x = hx - box_w - 8.0;
            if (box_x < 2.0)
                box_x = 2.0;
            if (box_y < margin_top)
                box_y = hy + 8.0;
            if (box_y + box_h > height - 2.0)
                box_y = height - box_h - 2.0;

            double hb[4];

            fmon_theme_hover_bg(hb);
            cairo_set_source_rgba(cr, hb[0], hb[1], hb[2], 0.94);
            cairo_rectangle(cr, box_x, box_y, box_w, box_h);
            cairo_fill(cr);

            cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 0.8);
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, box_x, box_y, box_w, box_h);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, cd->r, cd->g, cd->b, 1.0);
            cairo_move_to(cr, box_x + 7.0, box_y + 4.0 + ext.height);
            cairo_show_text(cr, label);
        }

        /* 峰值参考线：当前窗口的最大值（含包络） */
        if (have_range) {
            double peak_v = data_hi;
            double py_peak = margin_top + (double)plot_h -
                             ((peak_v - sc.lo) / (sc.hi - sc.lo)) *
                                 (double)plot_h;
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
            char pval[48];

            gui_chart_format_value(pval, sizeof(pval), peak_v, cd->unit);
            snprintf(pbuf, sizeof(pbuf), "峰值 %s", pval);

            cairo_text_extents_t pext;

            cairo_text_extents(cr, pbuf, &pext);

            double lx = margin_left + plot_w - pext.width - 6.0;

            if (lx < margin_left + 2.0)
                lx = margin_left + 2.0;

            double ly = py_peak - 5.0;

            if (ly - pext.height < margin_top + 1.0)
                ly = py_peak + pext.height + 6.0;
            if (ly > margin_top + plot_h - 2.0)
                ly = margin_top + plot_h - 2.0;

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

    g_free(band_lo);
    g_free(band_hi);
    g_free(pts);
}
