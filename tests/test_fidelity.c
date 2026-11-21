/*
 * test_fidelity.c - “数据不失真”契约测试
 *
 * 只测试纯几何函数，不依赖 GTK/Cairo：
 *   - 孤立尖峰在按像素列降采样后仍保留
 *   - NaN 缺口不会被补成 0
 *   - 输出 x 单调，首尾不丢
 *   - 固定量程不会被静默裁剪
 */
#include <glib.h>
#include <math.h>
#include <stddef.h>

#include "chart_geom.h"

static void test_isolated_spike(void)
{
    enum { N = 5000 };
    static double h[N];
    FmonChartPoint pts[256];

    for (int i = 0; i < N; i++)
        h[i] = 1.0;
    h[3217] = 99.0;

    size_t n = fmon_chart_polyline(h, N, N, N, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   100.0, 100, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, >, 0);

    double min_y = 1e9;
    for (size_t i = 0; i < n; i++)
        if (pts[i].y < min_y)
            min_y = pts[i].y;

    /* 99% 的峰必须接近 y=0（轴顶），不能被平均掉。 */
    g_assert_cmpfloat(min_y, <, 2.0);
}

static void test_nan_gap_not_zero(void)
{
    double h[8] = { 10.0, 20.0, NAN, NAN, 30.0, 40.0, 50.0, 60.0 };
    FmonChartPoint pts[16];

    size_t n = fmon_chart_polyline(h, 8, 8, 8, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   100.0, 0, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, >=, 6);

    bool saw_gap = false;
    for (size_t i = 0; i < n; i++)
        if (pts[i].gap)
            saw_gap = true;

    g_assert_true(saw_gap);
}

static void test_output_x_monotonic(void)
{
    enum { N = 1000 };
    static double h[N];
    FmonChartPoint pts[128];

    for (int i = 0; i < N; i++)
        h[i] = (double)(i % 37);

    size_t n = fmon_chart_polyline(h, N, N, N, 0.0, 100.0, 0.0, 100.0, 0.0,
                                   100.0, 80, pts, G_N_ELEMENTS(pts));

    g_assert_cmpuint(n, >, 0);
    for (size_t i = 1; i < n; i++)
        g_assert_cmpfloat(pts[i].x, >=, pts[i - 1].x);
}


static void test_step_path_orthogonal(void)
{
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
    cairo_t *cr = cairo_create(surface);

    FmonChartPoint pts[4] = {
        { .x = 10, .y = 80 },
        { .x = 30, .y = 40 },
        { .x = 60, .y = 70 },
        { .x = 90, .y = 20 },
    };

    fmon_chart_cairo_step_path(cr, pts, 4);

    cairo_path_t *path = cairo_copy_path(cr);

    g_assert_cmpint(path->status, ==, CAIRO_STATUS_SUCCESS);

    double cx = 0, cy = 0;

    for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
        cairo_path_data_t *d = &path->data[i];

        if (d->header.type == CAIRO_PATH_MOVE_TO) {
            cx = d[1].point.x;
            cy = d[1].point.y;
        } else if (d->header.type == CAIRO_PATH_LINE_TO) {
            double nx = d[1].point.x;
            double ny = d[1].point.y;

            /* 只允许水平或垂直，不能出现斜线/Bézier。 */
            g_assert_true(fabs(nx - cx) < 1e-9 || fabs(ny - cy) < 1e-9);
            cx = nx;
            cy = ny;
        } else {
            g_assert_cmpint(d->header.type, !=, CAIRO_PATH_CURVE_TO);
        }
    }

    cairo_path_destroy(path);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
static void test_scale_no_clip(void)
{
    FmonChartScale sc;

    fmon_chart_scale(0.0, 123.0, 0.0, 100.0, 1.0, 4, &sc);

    g_assert_cmpfloat(sc.lo, <=, 0.0);
    g_assert_cmpfloat(sc.hi, >=, 123.0);
}

static void test_fixed_range_still_expands(void)
{
    FmonChartScale sc;

    fmon_chart_scale(99.0, 101.0, 0.0, 100.0, 1.0, 4, &sc);

    g_assert_cmpfloat(sc.lo, <=, 99.0);
    g_assert_cmpfloat(sc.hi, >=, 101.0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/fidelity/spike", test_isolated_spike);
    g_test_add_func("/fidelity/nan-gap", test_nan_gap_not_zero);
    g_test_add_func("/fidelity/x-monotonic", test_output_x_monotonic);
    g_test_add_func("/fidelity/step/orthogonal",
                    test_step_path_orthogonal);
    g_test_add_func("/fidelity/scale/no-clip", test_scale_no_clip);
    g_test_add_func("/fidelity/scale/fixed-expands",
                    test_fixed_range_still_expands);

    return g_test_run();
}
