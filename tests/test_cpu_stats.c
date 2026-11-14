/*
 * test_cpu_stats.c - CPU 统计纯函数单测（时间占比/速率）
 */

#include <glib.h>

#include "cpu_stats.h"

static void test_pct(void)
{
    g_assert_cmpfloat_with_epsilon(cpu_stats_pct(50, 100), 50.0, 1e-9);
    g_assert_cmpfloat(cpu_stats_pct(0, 0), ==, 0.0);

    /* 越界值被钳制到 0–100 */
    g_assert_cmpfloat(cpu_stats_pct(200, 100), ==, 100.0);
}

static void test_rate(void)
{
    g_assert_cmpfloat_with_epsilon(cpu_stats_rate(150, 100, 2.0), 25.0, 1e-9);
    g_assert_cmpfloat(cpu_stats_rate(100, 100, 1.0), ==, 0.0);
    g_assert_cmpfloat(cpu_stats_rate(5, 100, 1.0), ==, 0.0);
    g_assert_cmpfloat(cpu_stats_rate(200, 100, 0.0), ==, 0.0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cpu_stats/pct", test_pct);
    g_test_add_func("/cpu_stats/rate", test_rate);
    return g_test_run();
}
