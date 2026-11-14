/*
 * test_zfs_stats.c - ZFS 细节的纯函数单元测试
 *
 * 覆盖命中率与速率计算：无访问、正常值、计数器回绕/重置、dt 非法。
 * sysctl 读取本身不做单测（依赖内核 ZFS 状态）。
 */

#include <glib.h>

#include "zfs_stats.h"

static void test_hit_pct(void)
{
    g_assert_cmpfloat_with_epsilon(zfs_stats_hit_pct(90, 10), 90.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(zfs_stats_hit_pct(1, 3), 25.0, 1e-9);
    g_assert_cmpfloat(zfs_stats_hit_pct(0, 0), ==, 0.0);
    g_assert_cmpfloat(zfs_stats_hit_pct(0, 5), ==, 0.0);
}

static void test_rate(void)
{
    g_assert_cmpfloat_with_epsilon(zfs_stats_rate(120, 100, 2.0), 10.0, 1e-9);
    g_assert_cmpfloat(zfs_stats_rate(100, 100, 1.0), ==, 0.0);

    /* 计数器重置/回绕：不能给出负数或巨大值 */
    g_assert_cmpfloat(zfs_stats_rate(5, 100, 1.0), ==, 0.0);

    /* 非法 dt */
    g_assert_cmpfloat(zfs_stats_rate(200, 100, 0.0), ==, 0.0);
    g_assert_cmpfloat(zfs_stats_rate(200, 100, -1.0), ==, 0.0);
}

static void test_reclaim_state(void)
{
    g_assert_cmpint(zfs_stats_reclaim_state(0, 0, 0, 0), ==,
                    ZFS_RECLAIM_IDLE);
    g_assert_cmpint(zfs_stats_reclaim_state(5, 0, 0, 0), ==,
                    ZFS_RECLAIM_NORMAL);
    g_assert_cmpint(zfs_stats_reclaim_state(0, 3, 0, 0), ==,
                    ZFS_RECLAIM_NORMAL);
    g_assert_cmpint(zfs_stats_reclaim_state(5, 0, 2, 0), ==,
                    ZFS_RECLAIM_PRESSURE);
    g_assert_cmpint(zfs_stats_reclaim_state(0, 0, 1, 1), ==,
                    ZFS_RECLAIM_IDLE);
    g_assert_cmpint(zfs_stats_reclaim_state(5, 0, 0, 1), ==,
                    ZFS_RECLAIM_PRESSURE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/zfs_stats/hit_pct", test_hit_pct);
    g_test_add_func("/zfs_stats/rate", test_rate);
    g_test_add_func("/zfs_stats/reclaim_state", test_reclaim_state);
    return g_test_run();
}

