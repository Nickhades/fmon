#include "cpu_health.h"

#include <glib.h>
#include <string.h>

static void test_ok(void)
{
    char      m[160];
    CpuHealth h = cpu_health_eval(10.0, 1.0, 16, 45.0, m, sizeof(m));

    g_assert_cmpint(h, ==, CPU_HEALTH_OK);
    g_assert_nonnull(strstr(m, "正常"));
}

static void test_warn_temp(void)
{
    char m[160];

    g_assert_cmpint(cpu_health_eval(10.0, 1.0, 16, 86.0, m, sizeof(m)), ==,
                    CPU_HEALTH_WARN);
}

static void test_bad_temp(void)
{
    char m[160];

    g_assert_cmpint(cpu_health_eval(10.0, 1.0, 16, 96.0, m, sizeof(m)), ==,
                    CPU_HEALTH_BAD);
}

static void test_bad_usage(void)
{
    char m[160];

    g_assert_cmpint(cpu_health_eval(96.0, 1.0, 16, 50.0, m, sizeof(m)), ==,
                    CPU_HEALTH_BAD);
}

static void test_warn_load(void)
{
    char m[160];

    g_assert_cmpint(cpu_health_eval(10.0, 20.0, 16, 50.0, m, sizeof(m)), ==,
                    CPU_HEALTH_WARN);
}

static void test_bad_load(void)
{
    char m[160];

    g_assert_cmpint(cpu_health_eval(10.0, 40.0, 16, 50.0, m, sizeof(m)), ==,
                    CPU_HEALTH_BAD);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/cpu_health/ok", test_ok);
    g_test_add_func("/cpu_health/warn_temp", test_warn_temp);
    g_test_add_func("/cpu_health/bad_temp", test_bad_temp);
    g_test_add_func("/cpu_health/bad_usage", test_bad_usage);
    g_test_add_func("/cpu_health/warn_load", test_warn_load);
    g_test_add_func("/cpu_health/bad_load", test_bad_load);

    return g_test_run();
}
