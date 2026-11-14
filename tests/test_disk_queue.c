/* iostat -x 解析测试 */

#include <glib.h>

#include "disk_queue.h"

static void test_basic(void)
{
    static const char TEXT[] =
        "                        extended device statistics  \n"
        "device       r/s     w/s     kr/s     kw/s  ms/r  ms/w  ms/o  ms/t qlen  %b  \n"
        "nda0          21      48    541.4   4176.7     0     0     1     0    0   0 \n"
        "pass0          0       0      0.0      0.0     0     0     0     0    0   0 \n";

    DiskQueue q = { 0 };

    g_assert_true(disk_queue_parse(TEXT, &q));
    g_assert_cmpint(q.n, ==, 1);
    g_assert_cmpstr(q.dev[0].name, ==, "nda0");
    g_assert_cmpfloat(q.dev[0].qlen, ==, 0.0);
}

static void test_queue_nonzero(void)
{
    static const char TEXT[] =
        "device       r/s     w/s     kr/s     kw/s  ms/r  ms/w  ms/o  ms/t qlen  %b  \n"
        "nda0         120     300   1200.0   9000.0     1     2     0     1    7  85 \n";

    DiskQueue q = { 0 };

    g_assert_true(disk_queue_parse(TEXT, &q));
    g_assert_cmpfloat(q.dev[0].qlen, ==, 7.0);
    g_assert_cmpfloat(q.dev[0].busy_pct, ==, 85.0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/disk_queue/basic", test_basic);
    g_test_add_func("/disk_queue/nonzero", test_queue_nonzero);
    return g_test_run();
}
