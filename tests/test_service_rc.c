/*
 * test_service_rc.c - rc.conf 解析单元测试
 *
 * 覆盖：注释/空行跳过、双引号/单引号/裸值、行尾注释、引号内 #、
 * 后出现的变量覆盖先出现的、_enable 的 YES/1/TRUE 判定。
 */

#include <glib.h>

#include "service_rc.h"

static GHashTable *parse(const char *text)
{
    GHashTable *vars =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    svc_rc_parse_text(vars, text);
    return vars;
}

static void test_quotes_and_comments(void)
{
    const char *text =
        "# 整行注释\n"
        "\n"
        "   \n"
        "sshd_enable=\"YES\"\n"
        "ntpd_enable='NO'      # 行尾注释\n"
        "dumpdev_enable=YES    # 裸值 + 注释\n"
        "qux = \"1\"\n"
        "sshd_enable=\"NO\"\n";      /* 后出现的覆盖前面的 */

    GHashTable *vars = parse(text);

    g_assert_false(svc_rc_enabled(vars, "sshd"));
    g_assert_false(svc_rc_enabled(vars, "ntpd"));
    g_assert_true(svc_rc_enabled(vars, "dumpdev"));
    g_assert_cmpstr(g_hash_table_lookup(vars, "qux"), ==, "1");
    g_assert_false(svc_rc_enabled(vars, "missing"));

    g_hash_table_destroy(vars);
}

static void test_hash_inside_quotes(void)
{
    /* 引号内的 # 是值的一部分，不能被当注释截断 */
    GHashTable *vars = parse("weird_enable=\"YES#NO\"\n");

    g_assert_cmpstr(g_hash_table_lookup(vars, "weird_enable"), ==, "YES#NO");
    g_assert_false(svc_rc_enabled(vars, "weird"));

    g_hash_table_destroy(vars);
}

static void test_enable_values(void)
{
    const char *text =
        "a_enable=yes\n"
        "b_enable=TRUE\n"
        "c_enable=1\n"
        "d_enable=no\n"
        "e_enable=\"\"\n";

    GHashTable *vars = parse(text);

    g_assert_true(svc_rc_enabled(vars, "a"));
    g_assert_true(svc_rc_enabled(vars, "b"));
    g_assert_true(svc_rc_enabled(vars, "c"));
    g_assert_false(svc_rc_enabled(vars, "d"));
    g_assert_false(svc_rc_enabled(vars, "e"));

    g_hash_table_destroy(vars);
}

static void test_malformed_lines(void)
{
    /* 没有 '='、空 key、空文本都不能崩，也不能污染已有变量 */
    GHashTable *vars = parse("novalue\n=oops\nok_enable=\"YES\"\n");

    g_assert_true(svc_rc_enabled(vars, "ok"));
    g_assert_cmpuint(g_hash_table_size(vars), ==, 1);

    svc_rc_parse_text(vars, NULL);
    svc_rc_parse_text(NULL, "x=1\n");

    g_hash_table_destroy(vars);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/service_rc/quotes", test_quotes_and_comments);
    g_test_add_func("/service_rc/hash_in_quotes", test_hash_inside_quotes);
    g_test_add_func("/service_rc/enable_values", test_enable_values);
    g_test_add_func("/service_rc/malformed", test_malformed_lines);
    return g_test_run();
}
