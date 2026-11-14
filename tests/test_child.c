/*
 * test_child.c - fmon_child 单元测试
 *
 * 覆盖：正常输出、退出码、超时、可执行文件不存在、输出上限截断、stderr 合并。
 */

#include <glib.h>
#include <sys/wait.h>

#include "fmon_child.h"

static void test_echo(void)
{
    const char *argv[] = { "/bin/echo", "hello", "world", NULL };
    FmonChildResult res;

    g_assert_cmpint(fmon_child_run(argv, 4096, 2000, &res), ==, 0);
    g_assert_true(fmon_child_ok(&res));
    g_assert_nonnull(res.out);
    g_assert_cmpstr(res.out, ==, "hello world\n");
    g_assert_false(res.timed_out);

    fmon_child_result_free(&res);
}

static void test_exit_code(void)
{
    const char *argv[] = { "/bin/sh", "-c", "exit 3", NULL };
    FmonChildResult res;

    g_assert_cmpint(fmon_child_run(argv, 1024, 2000, &res), ==, 0);
    g_assert_false(fmon_child_ok(&res));
    g_assert_true(WIFEXITED(res.status));
    g_assert_cmpint(WEXITSTATUS(res.status), ==, 3);

    fmon_child_result_free(&res);
}

static void test_timeout(void)
{
    const char *argv[] = { "/bin/sleep", "5", NULL };
    FmonChildResult res;

    g_assert_cmpint(fmon_child_run(argv, 1024, 200, &res), ==, 0);
    g_assert_true(res.timed_out);
    g_assert_false(fmon_child_ok(&res));

    fmon_child_result_free(&res);
}

static void test_missing_binary(void)
{
    const char *argv[] = { "/nonexistent/fmon-test-binary", NULL };
    FmonChildResult res;

    (void)fmon_child_run(argv, 1024, 1000, &res);
    g_assert_false(fmon_child_ok(&res));

    fmon_child_result_free(&res);
}

static void test_truncation(void)
{
    /* jot 会输出 10 万个 "x\n"，远超 1024 字节上限 */
    const char *argv[] = { "/usr/bin/jot", "-b", "x", "100000", NULL };
    FmonChildResult res;

    g_assert_cmpint(fmon_child_run(argv, 1024, 5000, &res), ==, 0);
    g_assert_true(res.truncated);
    g_assert_cmpuint(res.out_len, <=, 1024);

    fmon_child_result_free(&res);
}

static void test_stderr_merged(void)
{
    const char *argv[] = { "/bin/sh", "-c", "echo out; echo err 1>&2", NULL };
    FmonChildResult res;

    g_assert_cmpint(fmon_child_run_merged(argv, 4096, 2000, &res), ==, 0);
    g_assert_nonnull(res.out);
    g_assert_nonnull(strstr(res.out, "out"));
    g_assert_nonnull(strstr(res.out, "err"));

    fmon_child_result_free(&res);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/child/echo", test_echo);
    g_test_add_func("/child/exit_code", test_exit_code);
    g_test_add_func("/child/timeout", test_timeout);
    g_test_add_func("/child/missing_binary", test_missing_binary);
    g_test_add_func("/child/truncation", test_truncation);
    g_test_add_func("/child/stderr_merged", test_stderr_merged);

    return g_test_run();
}
