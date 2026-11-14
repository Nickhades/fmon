/*
 * test_kmem_stats.c - vmstat -z 解析归类
 */

#include <glib.h>

#include "kmem_stats.h"

static const char SAMPLE[] =
    "ITEM                   SIZE   LIMIT     USED     FREE      REQ FAIL SLEEP XDOM\n"
    "kstack_cache:         16384,      0,    1479,      77,    5430,   0,   0,   0\n"
    "zio_cache:             1152,      0,      20,    7638, 1765746,   0,   0,   0\n"
    "zio_buf_comb_512:       512,      0,     100,       0,       0,   0,   0,   0\n"
    "zfs_znode_cache:       1024,      0,      10,       0,       0,   0,   0,   0\n"
    "vm_page_t:              256,      0,      40,       0,       0,   0,   0,   0\n"
    "dnode_t:                760,      0,      10,       0,       0,   0,   0,   0\n"
    "vm pgcache:            4096,      0, 4194304,       0,       0,   0,   0,   0\n";

static void test_parse(void)
{
    KmemSummary s;

    g_assert_true(kmem_stats_parse(SAMPLE, &s));
    g_assert_cmpuint(s.zone_count, ==, 6);   /* vm pgcache 被跳过 */

    g_assert_cmpuint(s.zio_bytes, ==, 20 * 1152 + 100 * 512);
    g_assert_cmpuint(s.zfs_bytes, ==, 10 * 1024 + 10 * 760);
    g_assert_cmpuint(s.page_bytes, ==, 40 * 256);
    g_assert_cmpuint(s.total_bytes, ==, 1479 * 16384 + 20 * 1152 +
                                         100 * 512 + 10 * 1024 +
                                         40 * 256 + 10 * 760);
}

static void test_empty(void)
{
    KmemSummary s;

    g_assert_false(kmem_stats_parse("ITEM SIZE LIMIT USED\n", &s));
    g_assert_false(kmem_stats_parse(NULL, &s));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/kmem_stats/parse", test_parse);
    g_test_add_func("/kmem_stats/empty", test_empty);
    return g_test_run();
}
