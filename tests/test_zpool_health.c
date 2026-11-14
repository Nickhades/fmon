/*
 * test_zpool_health.c - zpool status / list 解析
 */

#include <glib.h>

#include "zpool_health.h"

static const char STATUS[] =
    "  pool: zroot\n"
    " state: ONLINE\n"
    "  scan: scrub repaired 0B in 00:12:34 with 0 errors on Sun Nov 10 03:00:00 2026\n"
    "config:\n"
    "\n"
    "        NAME        STATE     READ WRITE CKSUM\n"
    "        zroot       ONLINE       0     0     0\n"
    "          nvd0p4    ONLINE       0     0     0\n"
    "\n"
    "errors: No known data errors\n";

static const char STATUS_BAD[] =
    "  pool: tank\n"
    " state: DEGRADED\n"
    "config:\n"
    "        NAME        STATE     READ WRITE CKSUM\n"
    "        tank        DEGRADED     1     2     3\n"
    "          ada0      ONLINE       1     2     3\n";

static void test_status(void)
{
    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    h.frag_pct = -1;
    g_assert_true(zpool_health_parse_status(STATUS, &h));
    g_assert_cmpstr(h.pool, ==, "zroot");
    g_assert_cmpstr(h.state, ==, "ONLINE");
    g_assert_true(h.has_scan);
    g_assert_nonnull(strstr(h.scan, "scrub repaired"));
    g_assert_cmpuint(h.read_err, ==, 0);
    g_assert_cmpuint(h.cksum_err, ==, 0);
}

static void test_status_errors(void)
{
    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    h.frag_pct = -1;
    g_assert_true(zpool_health_parse_status(STATUS_BAD, &h));
    g_assert_cmpstr(h.pool, ==, "tank");
    g_assert_cmpuint(h.read_err, ==, 2);
    g_assert_cmpuint(h.write_err, ==, 4);
    g_assert_cmpuint(h.cksum_err, ==, 6);
}

static void test_frag(void)
{
    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    h.frag_pct = -1;
    g_assert_true(zpool_health_parse_frag("zroot\t7\n", &h));
    g_assert_cmpstr(h.pool, ==, "zroot");
    g_assert_cmpint(h.frag_pct, ==, 7);

    g_assert_false(zpool_health_parse_frag("", &h));
}

static void test_iostat(void)
{
    static const char LINE[] =
        "zroot\t68.1G\t376G\t54\t25\t1.30M\t1.27M\t135us\t2ms\t121us"
        "\t78us\t4us\t14us\t53us\t2ms\t2ms\t-\t-\t0\t0\t0\t0\t0\t0"
        "\t0\t0\t0\t0\t0\t0\t0\t0\n";

    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    g_assert_true(zpool_health_parse_iostat(LINE, &h));
    g_assert_cmpfloat_with_epsilon(h.total_wait_r_ms, 0.135, 1e-9);
    g_assert_cmpfloat_with_epsilon(h.total_wait_w_ms, 2.0, 1e-9);
    g_assert_cmpfloat_with_epsilon(h.disk_wait_r_ms, 0.121, 1e-9);
    g_assert_cmpfloat_with_epsilon(h.disk_wait_w_ms, 0.078, 1e-9);
    g_assert_cmpfloat(h.q_pend, ==, 0.0);
    g_assert_cmpfloat(h.q_activ, ==, 0.0);

    g_assert_false(zpool_health_parse_iostat("zroot\t1\n", &h));
}

static void test_vdevs(void)
{
    static const char TEXT[] =
        "zroot\t76405160960\t400336208896\t52\t26\t1346620\t1434798\n"
        "nda0p3\t76405160960\t400336208896\t52\t26\t1346620\t1434798\n";

    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    g_assert_cmpint(zpool_health_parse_vdevs(TEXT, &h), ==, 1);
    g_assert_cmpstr(h.pool, ==, "zroot");
    g_assert_cmpstr(h.vdev[0].name, ==, "nda0p3");
    g_assert_cmpfloat(h.vdev[0].rops, ==, 52.0);
    g_assert_cmpfloat(h.vdev[0].wops, ==, 26.0);
    g_assert_cmpfloat(h.vdev[0].rbps, ==, 1346620.0);
}

static void test_latency(void)
{
    static const char TEXT[] =
        "latency      read  write   read  write   read  write   read  write  scrub   trim  rebuild\n"
        "1ns             0      0      0      0  3.43K     19    258      4      0      0      0\n"
        "3ns             0      0      0      0      0      0      0      0      0      0      0\n";

    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    g_assert_true(zpool_health_parse_latency(TEXT, &h));
    g_assert_cmpint(h.lat_n, ==, 2);
    g_assert_cmpstr(h.lat[0].label, ==, "1ns");
    g_assert_cmpfloat_with_epsilon(h.lat[0].count, 3711.0, 1e-6);
}

static void test_reqsize(void)
{
    static const char TEXT[] =
        "req_size      ind    agg    ind    agg    ind    agg    ind    agg\n"
        "512         21.0K      0      0      0      2      0  15.7K      0\n"
        "4K          16.4K     14  2.51K      0    827     10  21.5K  14.8K\n"
        "64K         9.26K    115    164     30  5.70K    574  1.21K  8.16K\n"
        "256K            0      0      0      0      0      0      0      0\n";

    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    g_assert_true(zpool_health_parse_reqsize(TEXT, &h));
    g_assert_cmpfloat_with_epsilon(h.req_small,
                                   15700.0 + 2510 + 21500 + 14800, 1.0);
    g_assert_cmpfloat_with_epsilon(h.req_mid,
                                   164.0 + 30 + 1210 + 8160, 1.0);
    g_assert_cmpfloat(h.req_large, ==, 0.0);
}

static void test_txg(void)
{
    static const char TEXT[] =
        "txg      birth            state ndirty       nread        nwritten     reads    writes   otime        qtime        wtime        stime       \n"
        "44655    472832623466     C     2427904      0            1104384      0        105      4999265542   2375         5936         5893764     \n"
        "44754    969856900444     O     0            0            0            0        0        0            0            0            0           \n";

    ZpoolHealth h;

    memset(&h, 0, sizeof(h));
    g_assert_true(zpool_health_parse_txg(TEXT, &h));
    g_assert_cmpuint(h.txg, ==, 44655);
    g_assert_cmpfloat_with_epsilon(h.stime_ms, 5893.764, 1e-6);
    g_assert_cmpfloat_with_epsilon(h.txg_qtime_ms, 2.375, 1e-6);
    g_assert_cmpfloat_with_epsilon(h.txg_wtime_ms, 5.936, 1e-6);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/zpool_health/status", test_status);
    g_test_add_func("/zpool_health/errors", test_status_errors);
    g_test_add_func("/zpool_health/frag", test_frag);
    g_test_add_func("/zpool_health/iostat", test_iostat);
    g_test_add_func("/zpool_health/vdevs", test_vdevs);
    g_test_add_func("/zpool_health/txg", test_txg);
    g_test_add_func("/zpool_health/latency", test_latency);
    g_test_add_func("/zpool_health/reqsize", test_reqsize);
    return g_test_run();
}



