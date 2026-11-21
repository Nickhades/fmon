/*
 * test_log_filter.c - log_filter.c 纯函数测试
 */

#include "log_filter.h"

#include <glib.h>
#include <string.h>
#include <time.h>

static void test_rank(void)
{
    g_assert_cmpint(fmon_log_syslog_rank("<11>foo"), ==, 3);
    g_assert_cmpint(fmon_log_syslog_rank("kernel: panic"), ==, 0);
    g_assert_cmpint(fmon_log_syslog_rank("foo warning bar"), ==, 4);
    g_assert_cmpint(fmon_log_syslog_rank("plain message"), ==, 6);
}

static void test_level(void)
{
    g_assert_true(fmon_log_level_matches("anything", LOG_FILTER_ALL));
    g_assert_true(fmon_log_level_matches("disk error", LOG_FILTER_ERR));
    g_assert_false(fmon_log_level_matches("just notice", LOG_FILTER_ERR));
}

static void test_range(void)
{
    g_assert_true(fmon_log_in_range("no timestamp", 3600));

    time_t now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);

    char iso[64];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);

    g_assert_true(fmon_log_in_range(iso, 3600));
    g_assert_false(fmon_log_in_range("2000-01-01T00:00:00Z", 3600));
}

static void test_pattern(void)
{
    GPtrArray *pats = g_ptr_array_new();

    g_ptr_array_add(pats, (gpointer)"error");
    g_ptr_array_add(pats, (gpointer)"disk");

    g_assert_true(fmon_log_pattern_match("Disk ERROR occurred", pats,
                                         FALSE, FALSE));
    g_assert_false(fmon_log_pattern_match("disk ok", pats, FALSE, FALSE));

    g_ptr_array_set_size(pats, 0);
    g_ptr_array_add(pats, (gpointer)"^ada[0-9]+");
    g_assert_true(fmon_log_pattern_match("ada0: timeout", pats, TRUE, TRUE));

    g_ptr_array_free(pats, TRUE);
}

static void test_dedup(void)
{
    gchar *a = fmon_log_dedup_key("Jan  1 host prog[12]: same text");
    gchar *b = fmon_log_dedup_key("Jan  2 host prog[99]: same text");

    g_assert_cmpstr(a, ==, "same text");
    g_assert_cmpstr(b, ==, "same text");
    g_free(a);
    g_free(b);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/log/rank", test_rank);
    g_test_add_func("/log/level", test_level);
    g_test_add_func("/log/range", test_range);
    g_test_add_func("/log/pattern", test_pattern);
    g_test_add_func("/log/dedup", test_dedup);

    return g_test_run();
}
