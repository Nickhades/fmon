/*
 * test_chart.c - 折线图几何 / 格式化 / 离屏渲染测试
 *
 * 这些测试不需要 GTK 或 X11：量程、缺口、抽取、格式化是纯函数；
 * 渲染测试使用 Cairo image surface，能在 CI / SSH 里跑。
 */

#include "chart_geom.h"
#include "gui_chart_util.h"

#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <string.h>

static void assert_near(double got, double want)
{
    g_assert_cmpfloat(fabs(got - want), <, 1e-6);
}

static void test_nice_scale_percent(void)
{
    FmonChartScale sc;

    g_assert_true(fmon_chart_scale(0.0, 99.0, 0.0, 100.0, 1.0, 4, &sc));
    assert_near(sc.lo, 0.0);
    assert_near(sc.hi, 100.0);
    assert_near(sc.step, 25.0);
    g_assert_cmpint(sc.intervals, ==, 4);
}

static void test_scale_never_clips_peak(void)
{
    FmonChartScale sc;

    /* 固定 0-100 是基准；真实值 123 超出时必须扩展轴，而不是裁掉。 */
    g_assert_true(fmon_chart_scale(0.0, 123.0, 0.0, 100.0, 1.0, 4, &sc));
    g_assert_cmpfloat(sc.hi, >=, 123.0);
    g_assert_cmpfloat(sc.lo, <=, 0.0);
    g_assert_cmpfloat((sc.hi - sc.lo) / sc.step,
                      >, (double)(sc.intervals - 1));
}

static void test_scale_auto_min_keeps_tight_range(void)
{
    FmonChartScale sc;

    g_assert_true(fmon_chart_scale(98.2, 99.9, -1.0, 100.0, 1.0, 4, &sc));
    g_assert_cmpfloat(sc.lo, <=, 98.2);
    g_assert_cmpfloat(sc.hi, >=, 99.9);
    g_assert_cmpfloat(sc.hi - sc.lo, >=, 1.0);
}

static void test_scale_constant(void)
{
    FmonChartScale sc;

    g_assert_true(fmon_chart_scale(42.8, 42.8, -1.0, 0.0, 0.0, 4, &sc));
    g_assert_cmpfloat(sc.lo, <, 42.8);
    g_assert_cmpfloat(sc.hi, >, 42.8);
}

static void test_scale_zero_nonnegative(void)
{
    FmonChartScale sc;

    g_assert_true(fmon_chart_scale(0.0, 0.0, 0.0, 0.0, 0.0, 4, &sc));
    assert_near(sc.lo, 0.0);
    g_assert_cmpfloat(sc.hi, >, 0.0);
}

static void test_scale_fixed_range_kept(void)
{
    FmonChartScale sc;

    /* 两端固定时 min_span 不能把轴撑出 0..0.5（更不能变成负数）。 */
    g_assert_true(fmon_chart_scale(0.2, 0.3, 0.0, 0.5, 1.0, 4, &sc));
    g_assert_cmpfloat(sc.lo, >=, 0.0);
    g_assert_cmpfloat(sc.hi, <=, 0.5);
    g_assert_cmpfloat(sc.hi, >, 0.3);
}

static void test_history_range_skips_nan(void)
{
    double h[6] = { NAN, 2.0, NAN, 8.0, 4.0, NAN };
    double lo = 0.0, hi = 0.0;

    g_assert_true(fmon_chart_history_range(h, 6, 6, 6, &lo, &hi));
    assert_near(lo, 2.0);
    assert_near(hi, 8.0);

    double all_nan[3] = { NAN, NAN, NAN };

    g_assert_false(fmon_chart_history_range(all_nan, 3, 3, 3, &lo, &hi));
}

static void test_history_ring_wrap(void)
{
    double h[5] = { 40.0, 50.0, 10.0, 20.0, 30.0 };
    double v = 0.0;

    /* index=2 表示下一个写入位置；有效 5 点从槽位 2 开始。 */
    g_assert_true(fmon_chart_history_value(h, 5, 5, 2, 0, &v));
    assert_near(v, 10.0);

    g_assert_true(fmon_chart_history_value(h, 5, 5, 2, 4, &v));
    assert_near(v, 50.0);

    g_assert_false(fmon_chart_history_value(h, 5, 5, 2, 5, &v));

    /* count 超过容量时只取最近 capacity 个点。 */
    g_assert_true(fmon_chart_history_value(h, 5, 9, 2, 0, &v));
    assert_near(v, 10.0);
}

static void test_polyline_linear(void)
{
    double h[3] = { 1.0, 2.0, 3.0 };
    FmonChartPoint pts[8];
    size_t n = fmon_chart_polyline(h, 3, 3, 3, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   4.0, 100, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, ==, 3);
    assert_near(pts[0].x, 0.0);
    assert_near(pts[0].y, 75.0);
    assert_near(pts[1].x, 50.0);
    assert_near(pts[1].y, 50.0);
    assert_near(pts[2].x, 100.0);
    assert_near(pts[2].y, 25.0);
    g_assert_false(pts[1].gap);
    g_assert_false(pts[2].gap);
}

static void test_polyline_gap(void)
{
    double h[3] = { 1.0, NAN, 3.0 };
    FmonChartPoint pts[8];
    size_t n = fmon_chart_polyline(h, 3, 3, 3, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   4.0, 100, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, ==, 2);
    assert_near(pts[0].x, 0.0);
    assert_near(pts[1].x, 100.0);
    g_assert_true(pts[1].gap);
}

static void test_polyline_decimation_keeps_peak(void)
{
    enum { N = 1000 };
    static double h[N];
    FmonChartPoint pts[N + 8];

    for (int i = 0; i < N; i++)
        h[i] = 0.0;
    h[500] = 10.0;

    size_t n = fmon_chart_polyline(h, N, N, N, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   10.0, 100, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, >, 0);
    g_assert_cmpuint(n, <=, N + 8);

    double min_y = 1e9;
    double first_x = -1.0;
    double last_x = -1.0;

    for (size_t i = 0; i < n; i++) {
        if (pts[i].y < min_y)
            min_y = pts[i].y;
        if (first_x < 0.0)
            first_x = pts[i].x;
        last_x = pts[i].x;
    }

    /* 尖峰必须落在 y=0（轴顶），不能因为抽取被抹成 0 值。 */
    assert_near(min_y, 0.0);
    assert_near(first_x, 0.0);
    assert_near(last_x, 100.0);
}

static void test_polyline_ring_wrap(void)
{
    double h[5] = { 40.0, 50.0, 10.0, 20.0, 30.0 };
    FmonChartPoint pts[8];
    size_t n = fmon_chart_polyline(h, 5, 5, 2, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   50.0, 100, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, ==, 5);
    assert_near(pts[0].y, 80.0);  /* 10 */
    assert_near(pts[4].y, 0.0);   /* 50 = 轴顶 */
}

static void test_time_mapping(void)
{
    double h[4] = { 1.0, 2.0, 3.0, 4.0 };
    uint64_t t[4] = { 1000000, 2000000, 5000000, 6000000 };
    uint64_t first = 0, last = 0;
    FmonChartPoint pts[8];

    g_assert_true(fmon_chart_time_range(t, 4, 4, 4, &first, &last));
    g_assert_cmpuint(first, ==, 1000000);
    g_assert_cmpuint(last, ==, 6000000);

    size_t n = fmon_chart_polyline_at(
        h, t, 4, 4, 4, first, last, 0.0, 100.0, 0.0, 100.0, 0.0, 100.0, 100,
        2500000, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, ==, 4);
    assert_near(pts[0].x, 0.0);
    assert_near(pts[1].x, 20.0);
    assert_near(pts[2].x, 80.0);
    assert_near(pts[3].x, 100.0);
    g_assert_true(pts[2].gap);   /* 3s > 2.5s */
    g_assert_false(pts[1].gap);

    /* 缺失中间时间戳：下一个有效点也必须断开，不能从缺失点连出假线段。 */
    uint64_t bad_t[3] = { 1000000, 0, 3000000 };
    double   h3[3] = { 1.0, 2.0, 3.0 };
    FmonChartPoint p3[4];

    size_t m = fmon_chart_polyline_at(
        h3, bad_t, 3, 3, 3, 1000000, 3000000, 0.0, 100.0, 0.0, 100.0, 0.0,
        100.0, 100, 0, p3, G_N_ELEMENTS(p3));

    g_assert_cmpuint(m, ==, 3);
    g_assert_true(p3[1].gap);
    g_assert_true(p3[2].gap);
}

static void test_formatting(void)
{
    char buf[64];

    gui_chart_format_axis_label(buf, sizeof(buf), 0.25, "%", 0.25);
    g_assert_cmpstr(buf, ==, "0.25%");

    gui_chart_format_axis_label(buf, sizeof(buf), 1536.0, "KB/s", 512.0);
    g_assert_cmpstr(buf, ==, "1.50 MB/s");

    gui_chart_format_value(buf, sizeof(buf), NAN, "%");
    g_assert_cmpstr(buf, ==, "—");

    gui_chart_format_time_label(buf, sizeof(buf), 3600.0);
    g_assert_cmpstr(buf, ==, "-1h");

    gui_chart_format_offset(buf, sizeof(buf), 12.25, 0.25);
    g_assert_cmpstr(buf, ==, "-12.25s");

    gui_chart_format_offset(buf, sizeof(buf), 60.0, 1.0);
    g_assert_cmpstr(buf, ==, "-1m");
}

static void test_cairo_render_smoke(void)
{
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 100);
    cairo_t *cr = cairo_create(surface);

    g_assert_cmpint(cairo_surface_status(surface), ==, CAIRO_STATUS_SUCCESS);
    g_assert_cmpint(cairo_status(cr), ==, CAIRO_STATUS_SUCCESS);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    FmonChartPoint pts[4] = {
        { .x = 10.0, .y = 90.0, .gap = true, .src = 0 },
        { .x = 70.0, .y = 20.0, .gap = false, .src = 1 },
        { .x = 130.0, .y = 70.0, .gap = false, .src = 2 },
        { .x = 190.0, .y = 30.0, .gap = false, .src = 3 },
    };

    cairo_set_source_rgb(cr, 1.0, 0.2, 0.2);
    cairo_set_line_width(cr, 2.0);
    fmon_chart_cairo_path(cr, pts, G_N_ELEMENTS(pts));
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.2, 0.8, 0.3, 0.25);
    fmon_chart_cairo_fill(cr, pts, G_N_ELEMENTS(pts), 100.0);
    cairo_fill(cr);

    FmonChartPoint lower[3] = {
        { .x = 10.0, .y = 90.0, .gap = true, .src = 0 },
        { .x = 100.0, .y = 80.0, .gap = false, .src = 1 },
        { .x = 190.0, .y = 90.0, .gap = false, .src = 2 },
    };
    FmonChartPoint upper[3] = {
        { .x = 10.0, .y = 40.0, .gap = true, .src = 0 },
        { .x = 100.0, .y = 30.0, .gap = false, .src = 1 },
        { .x = 190.0, .y = 40.0, .gap = false, .src = 2 },
    };

    cairo_set_source_rgba(cr, 0.2, 0.3, 1.0, 0.35);
    fmon_chart_cairo_band(cr, lower, upper, G_N_ELEMENTS(lower));
    cairo_fill(cr);

    cairo_surface_flush(surface);
    g_assert_cmpint(cairo_status(cr), ==, CAIRO_STATUS_SUCCESS);

    unsigned char *data = cairo_image_surface_get_data(surface);
    int            stride = cairo_image_surface_get_stride(surface);
    int            nonblack = 0;

    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 200; x++) {
            unsigned char *p = data + y * stride + x * 4;

            if (p[0] || p[1] || p[2])
                nonblack++;
        }

    g_assert_cmpint(nonblack, >, 500);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/chart/scale/percent", test_nice_scale_percent);
    g_test_add_func("/chart/scale/no-clip", test_scale_never_clips_peak);
    g_test_add_func("/chart/scale/tight-min", test_scale_auto_min_keeps_tight_range);
    g_test_add_func("/chart/scale/constant", test_scale_constant);
    g_test_add_func("/chart/scale/zero", test_scale_zero_nonnegative);
    g_test_add_func("/chart/scale/fixed-range", test_scale_fixed_range_kept);
    g_test_add_func("/chart/history/nan", test_history_range_skips_nan);
    g_test_add_func("/chart/history/wrap", test_history_ring_wrap);
    g_test_add_func("/chart/polyline/linear", test_polyline_linear);
    g_test_add_func("/chart/polyline/gap", test_polyline_gap);
    g_test_add_func("/chart/polyline/decimation", test_polyline_decimation_keeps_peak);
    g_test_add_func("/chart/polyline/wrap", test_polyline_ring_wrap);
    g_test_add_func("/chart/time", test_time_mapping);
    g_test_add_func("/chart/format", test_formatting);
    g_test_add_func("/chart/render", test_cairo_render_smoke);

    return g_test_run();
}
