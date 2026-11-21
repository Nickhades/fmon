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

#include "gui_chart_internal.h"
#include "gui_chart_draw.h"

#include "chart_geom.h"
#include "gui_chart_util.h"
#include "gui_util.h"
#include "theme.h"
#include "alerts.h"

#include <glib/gstdio.h>

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


typedef struct {
    const double   *history;
    const uint64_t *time_us;
    const double   *history_min;
    const double   *history_max;
    uint32_t        count;
    uint32_t        index;
    uint32_t        max_cols;
    uint64_t        t0;
    uint64_t        t1;
    uint64_t        end_time_us;
    uint64_t        gap_us;
    int             margin_left;
    int             margin_top;
    int             plot_w;
    int             plot_h;
    double          lo;
    double          hi;

    FmonChartPoint *pts;
    size_t          n;
    FmonChartPoint *band_lo;
    FmonChartPoint *band_hi;
    size_t          band_n;
} ChartPointCache;

static void chart_point_cache_free(gpointer data)
{
    ChartPointCache *c = data;

    if (c == NULL)
        return;

    g_free(c->pts);
    g_free(c->band_lo);
    g_free(c->band_hi);
    g_free(c);
}

static ChartPointCache *chart_point_cache_get(GtkWidget *area)
{
    ChartPointCache *c =
        g_object_get_data(G_OBJECT(area), "fmon-chart-point-cache");

    if (c == NULL) {
        c = g_new0(ChartPointCache, 1);
        g_object_set_data_full(G_OBJECT(area), "fmon-chart-point-cache", c,
                               chart_point_cache_free);
    }

    return c;
}

static bool chart_point_cache_match(const ChartPointCache *c,
                                    const CurveData *cd, uint32_t points,
                                    uint64_t t0, uint64_t t1,
                                    uint64_t end_time, uint32_t max_cols,
                                    uint64_t gap_us, int margin_left,
                                    int margin_top, int plot_w, int plot_h,
                                    double lo, double hi)
{
    return c->pts != NULL && c->history == cd->history &&
           c->time_us == cd->time_us &&
           c->history_min == cd->history_min &&
           c->history_max == cd->history_max && c->count == points &&
           c->index == cd->index && c->max_cols == max_cols &&
           c->t0 == t0 && c->t1 == t1 && c->end_time_us == end_time &&
           c->gap_us == gap_us && c->margin_left == margin_left &&
           c->margin_top == margin_top && c->plot_w == plot_w &&
           c->plot_h == plot_h && c->lo == lo && c->hi == hi;
}

static void chart_point_cache_invalidate(ChartPointCache *c)
{
    g_free(c->pts);
    g_free(c->band_lo);
    g_free(c->band_hi);
    c->pts = NULL;
    c->band_lo = NULL;
    c->band_hi = NULL;
    c->n = 0;
    c->band_n = 0;
}

void gui_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data)
{
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

    uint64_t t0 = 0, t1 = 0;
    bool     has_time = gui_chart_view_range(cd, points, &t0, &t1);
    bool     time_window = has_time && gui_chart_window_us() > 0;

    double data_lo = 0.0;
    double data_hi = 0.0;
    bool   have_range = gui_chart_curve_range(cd, points, t0, t1, time_window, &data_lo,
                                    &data_hi);

    uint64_t gap_us = gui_chart_gap_threshold_us(cd);

    double min_span = (cd->unit != NULL && strcmp(cd->unit, "%") == 0) ? 1.0
                                                                       : 0.0;

    FmonChartScale sc;
    gboolean        auto_pct = gui_chart_auto_range_for(cd);

    fmon_chart_scale(have_range ? data_lo : 0.0, have_range ? data_hi : 0.0,
                     auto_pct ? -1.0
                              : ((cd->min_val >= 0.0) ? cd->min_val : -1.0),
                     auto_pct ? 0.0
                              : ((cd->max_val > 0.0) ? cd->max_val : 0.0),
                     min_span, 4, &sc);

    /* 轴标签宽度决定左边距，避免 “1.50 MB/s” 被裁掉。 */
    gui_chart_set_font(cr);

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

    gui_chart_draw_frame(cr, cd, &sc, margin_left, margin_top, plot_w, plot_h);
    gui_chart_draw_threshold(cr, cd, &sc, margin_left, margin_top, plot_w, plot_h);

    /* X 轴时间刻度：相对当前时刻；有时间戳时使用真实时间跨度。 */
    if (points >= 2 && plot_w >= 220) {
        double total_sec;

        if (has_time)
            total_sec = (double)(t1 - t0) / 1000000.0;
        else
            total_sec = (double)(points - 1) * (double)cd->interval_us /
                        1000000.0;

        uint64_t latest = has_time ? curve_latest_us(cd) : 0;
        double   offset = (latest > t1) ? (double)(latest - t1) / 1000000.0
                                        : 0.0;

        for (int t = 0; t <= 3; t++) {
            double frac = (double)t / 3.0;
            double secs = total_sec * (1.0 - frac) + offset;

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
        gui_chart_draw_hint(cr, margin_left, margin_top, plot_w, plot_h,
                  (cd->empty_hint != NULL) ? cd->empty_hint : "正在采集…");
        return;
    }

    if (!have_range) {
        gui_chart_draw_hint(cr, margin_left, margin_top, plot_w, plot_h,
                  (cd->empty_hint != NULL) ? cd->empty_hint : "暂无有效采样");
        return;
    }

    /* 点数可能多于像素列：按列保留极值，保证尖峰不丢。 */
    uint32_t max_cols = (cd->history_min != NULL && cd->history_max != NULL)
                            ? 0
                            : (uint32_t)plot_w;

    ChartPointCache *cache = chart_point_cache_get(GTK_WIDGET(area));
    bool cache_hit = chart_point_cache_match(
        cache, cd, points, t0, t1, cd->end_time_us, max_cols, gap_us,
        margin_left, margin_top, plot_w, plot_h, sc.lo, sc.hi);

    FmonChartPoint *pts;
    size_t          n;
    FmonChartPoint *band_lo;
    FmonChartPoint *band_hi;
    size_t          band_n;

    if (cache_hit) {
        pts = cache->pts;
        n = cache->n;
        band_lo = cache->band_lo;
        band_hi = cache->band_hi;
        band_n = cache->band_n;
    } else {
        chart_point_cache_invalidate(cache);

        size_t cap = (size_t)points + 4;

        cache->pts = g_new0(FmonChartPoint, cap);
        n = fmon_chart_polyline_at(
            cd->history, cd->time_us, FMON_HISTORY_POINTS, points, cd->index,
            t0, t1, margin_left, (double)plot_w, margin_top,
            (double)plot_h, sc.lo, sc.hi, max_cols, gap_us, cache->pts, cap);

        cache->n = n;

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
                cache->band_lo = lo;
                cache->band_hi = hi;
                cache->band_n = n;
            } else {
                g_free(lo);
                g_free(hi);
            }
        }

        cache->history = cd->history;
        cache->time_us = cd->time_us;
        cache->history_min = cd->history_min;
        cache->history_max = cd->history_max;
        cache->count = points;
        cache->index = cd->index;
        cache->max_cols = max_cols;
        cache->t0 = t0;
        cache->t1 = t1;
        cache->end_time_us = cd->end_time_us;
        cache->gap_us = gap_us;
        cache->margin_left = margin_left;
        cache->margin_top = margin_top;
        cache->plot_w = plot_w;
        cache->plot_h = plot_h;
        cache->lo = sc.lo;
        cache->hi = sc.hi;

        pts = cache->pts;
        band_lo = cache->band_lo;
        band_hi = cache->band_hi;
        band_n = cache->band_n;
    }

    if (n == 0) {
        gui_chart_draw_hint(cr, margin_left, margin_top, plot_w, plot_h,
                  (cd->empty_hint != NULL) ? cd->empty_hint : "暂无有效采样");
        return;
    }

    cairo_save(cr);
    cairo_rectangle(cr, margin_left, margin_top, plot_w, plot_h);
    cairo_clip(cr);

    /* 极值包络：无填充，只画两条细实线，保持终端风格的纯线条。 */
    if (band_n > 0) {
        cairo_set_line_width(cr, 0.7);
        cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
        fmon_chart_cairo_step_path(cr, band_hi, band_n);
        cairo_stroke(cr);
        fmon_chart_cairo_step_path(cr, band_lo, band_n);
        cairo_stroke(cr);
    }

    /* 主数据：阶梯线，1.2px，无渐变、无面积填充。 */
    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    fmon_chart_cairo_step_path(cr, pts, n);
    cairo_stroke(cr);

    /* 末端采样点 */
    cairo_arc(cr, pts[n - 1].x, pts[n - 1].y, 1.8, 0.0, 2.0 * M_PI);
    cairo_set_source_rgb(cr, cd->r, cd->g, cd->b);
    cairo_fill(cr);

    cairo_restore(cr);

    gui_chart_draw_threshold_markers(cr, cd, &sc, pts, n, margin_top, plot_h);

    /* 统计值最后画，避免被曲线/填充盖住。 */
    gui_chart_draw_stats(cr, cd, points, t0, t1, time_window, margin_left, margin_top,
               plot_w);
    gui_chart_draw_selection(cr, GTK_WIDGET(area), margin_left, margin_top,
                             plot_w, plot_h);

    if (auto_pct) {
        double axis_col[4];

        fmon_theme_axis_color(axis_col);
        gui_chart_set_font(cr);
        cairo_set_font_size(cr, 9.0);

        const char *tag = "自动量程";
        cairo_text_extents_t ext;

        cairo_text_extents(cr, tag, &ext);
        cairo_set_source_rgba(cr, axis_col[0], axis_col[1], axis_col[2],
                              0.60);
        cairo_move_to(cr, margin_left + plot_w - ext.width - 4.0,
                      margin_top + 11.0);
        cairo_show_text(cr, tag);
        gui_chart_set_font(cr);
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

            if (!gui_chart_nearest_sample_by_time(cd, points, t, &i)) {
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
            uint64_t latest = curve_latest_us(cd);
            double   off = (latest > t1) ? (double)(latest - t1) / 1000000.0
                                         : 0.0;
            secs = (double)(t1 - ti) / 1000000.0 + off;
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


}
