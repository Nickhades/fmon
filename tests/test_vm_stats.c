/*
 * test_vm_stats.c - 内存队列占比（纯函数）单元测试
 *
 * vm_stats_read() 依赖内核 sysctl，这里只测不依赖系统的部分：
 * 占比计算、总和为 0、计数之和超过 total 时的钳制。
 */

#include <glib.h>

#include "vm_stats.h"

static void test_fractions_normal(void)
{
    VmStats s = { 0 };

    s.total    = 1000;
    s.active   = 400;
    s.inactive = 300;
    s.laundry  = 100;
    s.wired    = 150;   /* 含 50 的 ARC */
    s.arc      = 50;
    s.free     = 50;

    double f[VM_SEG_COUNT];

    vm_stats_fractions(&s, f);

    g_assert_cmpfloat_with_epsilon(f[VM_SEG_ACTIVE], 0.40, 1e-9);
    g_assert_cmpfloat_with_epsilon(f[VM_SEG_INACTIVE], 0.30, 1e-9);
    g_assert_cmpfloat_with_epsilon(f[VM_SEG_WIRED], 0.10, 1e-9);
    g_assert_cmpfloat_with_epsilon(f[VM_SEG_CACHE], 0.05, 1e-9);
    g_assert_cmpfloat_with_epsilon(f[VM_SEG_FREE], 0.05, 1e-9);

    double sum = 0.0;

    for (int i = 0; i < VM_SEG_COUNT; i++)
        sum += f[i];

    g_assert_cmpfloat_with_epsilon(sum, 1.0, 1e-9);
}

static void test_fractions_zero_total(void)
{
    VmStats s = { 0 };
    double  f[VM_SEG_COUNT];

    s.active = 123;

    vm_stats_fractions(&s, f);

    for (int i = 0; i < VM_SEG_COUNT; i++)
        g_assert_cmpfloat(f[i], ==, 0.0);
}

static void test_fractions_overflow(void)
{
    /* 各队列之和超过 total（内核计数不同步时可能出现）：
     * 不允许画出条形越界，总和钳制在 1.0。 */
    VmStats s = { 0 };

    s.total    = 100;
    s.active   = 90;
    s.inactive = 90;
    s.free     = 90;

    double f[VM_SEG_COUNT];
    double sum = 0.0;

    vm_stats_fractions(&s, f);

    for (int i = 0; i < VM_SEG_COUNT; i++) {
        g_assert_cmpfloat(f[i], >=, 0.0);
        sum += f[i];
    }

    g_assert_cmpfloat_with_epsilon(sum, 1.0, 1e-9);
}

static void test_arc_clamped(void)
{
    /* ARC 比 Wired 还大（计数不同步）：不能出现负的常驻，
     * 相机取 wired 作为缓存、常驻为 0。 */
    VmStats s = { 0 };

    s.total = 1000;
    s.wired = 100;
    s.arc   = 300;

    guint64 b[VM_SEG_COUNT];

    vm_stats_segment_bytes(&s, b);

    g_assert_cmpuint(b[VM_SEG_WIRED], ==, 0);
    g_assert_cmpuint(b[VM_SEG_CACHE], ==, 100);

    double f[VM_SEG_COUNT];

    vm_stats_fractions(&s, f);
    g_assert_cmpfloat_with_epsilon(f[VM_SEG_CACHE], 0.10, 1e-9);
}

static void test_null(void)
{
    double f[VM_SEG_COUNT];

    vm_stats_fractions(NULL, f);

    for (int i = 0; i < VM_SEG_COUNT; i++)
        g_assert_cmpfloat(f[i], ==, 0.0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/vm_stats/fractions", test_fractions_normal);
    g_test_add_func("/vm_stats/zero_total", test_fractions_zero_total);
    g_test_add_func("/vm_stats/overflow", test_fractions_overflow);
    g_test_add_func("/vm_stats/arc_clamped", test_arc_clamped);
    g_test_add_func("/vm_stats/null", test_null);
    return g_test_run();
}
