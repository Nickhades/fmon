/*
 * test_mem_summary.c - 内存页说明行单元测试
 *
 * 覆盖：无 ZFS（arc=0）隐藏、低于阈值隐藏、阈值边界显示、
 * 常规数值与两处钳制（used 不为负、实际可用不超过总容量）、总容量为 0。
 */

#include <glib.h>

#include "mem_summary.h"

#define GIB (1024ULL * 1024ULL * 1024ULL)

static void test_no_zfs(void)
{
    gchar *s = gui_mem_summary_text(15 * GIB, 2 * GIB, 0);

    g_assert_nonnull(s);
    g_assert_cmpstr(s, ==, "");
    g_free(s);
}

static void test_below_threshold(void)
{
    /* 没装 ZFS / ARC 很小：整行隐藏，避免 "0.00 GB" 噪音 */
    gchar *s = gui_mem_summary_text(15 * GIB, 2 * GIB,
                                    MEM_SUMMARY_ARC_MIN_BYTES - 1);
    g_assert_cmpstr(s, ==, "");
    g_free(s);
}

static void test_threshold_boundary(void)
{
    gchar *s = gui_mem_summary_text(15 * GIB, 2 * GIB,
                                    MEM_SUMMARY_ARC_MIN_BYTES);
    g_assert_cmpstr(s, ==,
                    "已用 13.00 GB（其中 ZFS ARC 0.05 GB 可回收）"
                    "｜ 实际可用约 2.05 GB");
    g_free(s);
}

static void test_normal(void)
{
    gchar *s = gui_mem_summary_text(15 * GIB, 3 * GIB / 2, 9 * GIB / 4);

    g_assert_cmpstr(s, ==,
                    "已用 13.50 GB（其中 ZFS ARC 2.25 GB 可回收）"
                    "｜ 实际可用约 3.75 GB");
    g_free(s);
}

static void test_clamp(void)
{
    /* avail 超过 total：已用不能为负；实际可用不能超过总容量 */
    gchar *s = gui_mem_summary_text(15 * GIB, 16 * GIB, 2 * GIB);

    g_assert_cmpstr(s, ==,
                    "已用 0.00 GB（其中 ZFS ARC 2.00 GB 可回收）"
                    "｜ 实际可用约 15.00 GB");
    g_free(s);
}

static void test_effective_pct(void)
{
    /* 总 16 GiB，可用 1.5 GiB（free+inactive+cache），ARC 2.5 GiB：
     * 可回收 = 1.5 + 2.5 = 4 GiB → 有效使用率 = 75%。 */
    g_assert_cmpfloat_with_epsilon(
        gui_mem_effective_used_pct(16 * GIB, 3 * GIB / 2, 5 * GIB / 2),
        75.0, 1e-9);

    /* ARC 很大时仍在 0–100 之间 */
    double p = gui_mem_effective_used_pct(16 * GIB, 0, 16 * GIB);

    g_assert_cmpfloat(p, >=, 0.0);
    g_assert_cmpfloat(p, <=, 100.0);
    g_assert_cmpfloat_with_epsilon(p, 0.0, 1e-9);

    /* ARC 计数异常（> 总量）时钳制到 100% 可回收，不出现下溢 */
    g_assert_cmpfloat(
        gui_mem_effective_used_pct(8 * GIB, 0, 16 * GIB), ==, 0.0);

    /* 总量为 0：返回 0 而不是除零 */
    g_assert_cmpfloat(gui_mem_effective_used_pct(0, 0, 0), ==, 0.0);
}

static void test_buf_text(void)
{
    gchar *s = gui_mem_buf_text(0);

    g_assert_cmpstr(s, ==, "");
    g_free(s);

    s = gui_mem_buf_text(2106368);          /* 约 2.0 MB，和 top 的 Buf 一致 */
    g_assert_cmpstr(s, ==, "缓冲（Buf）2.0 MB");
    g_free(s);

    s = gui_mem_buf_text(2 * GIB);
    g_assert_cmpstr(s, ==, "缓冲（Buf）2.00 GB");
    g_free(s);
}

static void test_zero_total(void)
{
    gchar *s = gui_mem_summary_text(0, 0, 2 * GIB);

    g_assert_cmpstr(s, ==, "");
    g_free(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mem_summary/no_zfs", test_no_zfs);
    g_test_add_func("/mem_summary/below_threshold", test_below_threshold);
    g_test_add_func("/mem_summary/threshold", test_threshold_boundary);
    g_test_add_func("/mem_summary/normal", test_normal);
    g_test_add_func("/mem_summary/clamp", test_clamp);
    g_test_add_func("/mem_summary/zero_total", test_zero_total);
    g_test_add_func("/mem_summary/buf", test_buf_text);
    g_test_add_func("/mem_summary/effective_pct", test_effective_pct);
    return g_test_run();
}
