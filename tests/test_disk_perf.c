/*
 * test_disk_perf.c - nvmecontrol SMART 解析
 */

#include <glib.h>

#include "disk_perf.h"

static const char SAMPLE[] =
    "SMART/Health Information Log\n"
    "============================\n"
    " Critical Warning State:         0x00\n"
    " Available spare:                100\n"
    " Available spare threshold:      10\n"
    " Percentage used:                6\n"
    "Temperature:                    315 K, 41.85 C, 107.33 F\n";

static void test_smart_parse(void)
{
    double t = 0;
    int    spare = 0, wear = 0;

    g_assert_true(disk_perf_parse_smart(SAMPLE, &t, &spare, &wear));
    g_assert_cmpfloat_with_epsilon(t, 41.85, 1e-6);
    g_assert_cmpint(spare, ==, 100);
    g_assert_cmpint(wear, ==, 6);
}

static void test_smart_empty(void)
{
    double t;

    g_assert_false(disk_perf_parse_smart("", &t, NULL, NULL));
    g_assert_false(disk_perf_parse_smart(NULL, &t, NULL, NULL));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/disk_perf/smart", test_smart_parse);
    g_test_add_func("/disk_perf/empty", test_smart_empty);
    return g_test_run();
}
