/*
 * test_process_ps.c - ps 输出解析单元测试
 *
 * 覆盖：表头跳过、字段解析、uid 归类、重复 pid 去重、
 * 自身 ps 辅助进程过滤、指纹稳定性与变化检测。
 */

#include <glib.h>

#include <string.h>

#include <sys/param.h>
#include <sys/user.h>

#include "process_ps.h"

static const char *SAMPLE_OUTPUT =
    "  PID   UID USER       %CPU %MEM   RSS STAT COMMAND\n"
    "  123     0 root        1.5  0.1  1024 Ss   /sbin/init\n"
    "  456  1001 zxy         0.0  2.0  2048 S    sleep 300\n"
    "  789   500 daemon      0.2  0.3   512 I    /usr/sbin/syslogd -s\n"
    "  456  1001 zxy         0.0  2.0  2048 S    duplicate-should-skip\n"
    "  999  1001 zxy         0.0  0.0   100 S    env LC_NUMERIC=C ps -ax\n";

static GPtrArray *parse(const char *text)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(proc_ps_row_free);

    proc_parse_ps_rows(text, rows);
    return rows;
}

static void test_parse_fields(void)
{
    GPtrArray *rows = parse(SAMPLE_OUTPUT);

    /* 表头、重复 pid(456)、自身 ps 辅助进程(999) 都被跳过 */
    g_assert_cmpuint(rows->len, ==, 3);

    const PsRow *r0 = g_ptr_array_index(rows, 0);
    g_assert_cmpuint(r0->pid, ==, 123);
    g_assert_cmpuint(r0->uid, ==, 0);
    g_assert_cmpint(r0->state, ==, 'S');
    g_assert_cmpfloat_with_epsilon(r0->cpu, 1.5, 0.001);
    g_assert_cmpfloat_with_epsilon(r0->mem, 0.1, 0.001);
    g_assert_cmpuint(r0->rss_kb, ==, 1024);
    g_assert_cmpstr(r0->user, ==, "root");
    g_assert_cmpstr(r0->comm, ==, "/sbin/init");
    g_assert_true(r0->is_system);

    const PsRow *r1 = g_ptr_array_index(rows, 1);
    g_assert_cmpuint(r1->pid, ==, 456);
    g_assert_cmpstr(r1->comm, ==, "sleep 300");
    g_assert_false(r1->is_system);

    /* uid < 1000 的系统账户也归为系统进程 */
    const PsRow *r2 = g_ptr_array_index(rows, 2);
    g_assert_cmpuint(r2->pid, ==, 789);
    g_assert_true(r2->is_system);

    g_ptr_array_free(rows, TRUE);
}

static void test_category(void)
{
    g_assert_cmpuint(proc_classify_category("/usr/local/bin/kwin_x11", 1001,
                                             FALSE), ==, 2);
    g_assert_cmpuint(proc_classify_category(
                         "/usr/local/lib/firefox/firefox", 1001, FALSE),
                     ==, 0);
    g_assert_cmpuint(proc_classify_category("python3 -c pass", 1001, FALSE),
                     ==, 1);
    g_assert_cmpuint(proc_classify_category("sleep 10", 1001, FALSE), ==, 1);
    g_assert_cmpuint(proc_classify_category("/sbin/init", 0, TRUE), ==, 2);
    g_assert_cmpuint(proc_classify_category("daemon: syncthing", 1001, FALSE),
                     ==, 2);

    g_assert_cmpstr(proc_category_name(0), ==, "桌面");
    g_assert_cmpstr(proc_category_name(1), ==, "用户");
    g_assert_cmpstr(proc_category_name(2), ==, "系统");
}

static void test_fingerprint(void)
{
    GPtrArray *a = parse(SAMPLE_OUTPUT);
    GPtrArray *b = parse(SAMPLE_OUTPUT);
    GPtrArray *c = parse("  123     0 root        9.9  0.1  1024 Ss   /sbin/init\n");

    guint64 fa = proc_parse_ps_rows(SAMPLE_OUTPUT, a);
    guint64 fb = proc_parse_ps_rows(SAMPLE_OUTPUT, b);
    guint64 fc = proc_parse_ps_rows("  123     0 root        9.9  0.1  1024 Ss   /sbin/init\n", c);

    g_assert_cmpuint(fa, !=, 0);
    g_assert_cmpuint(fa, ==, fb);   /* 相同输入 → 相同指纹 */
    g_assert_cmpuint(fa, !=, fc);   /* CPU 变化 → 指纹变化 */

    g_ptr_array_free(a, TRUE);
    g_ptr_array_free(b, TRUE);
    g_ptr_array_free(c, TRUE);
}

static void test_null_input(void)
{
    GPtrArray *rows = parse(NULL);

    g_assert_cmpuint(rows->len, ==, 0);
    g_ptr_array_free(rows, TRUE);
}

static void test_kproc_aggregate(void)
{
    /* FreeBSD 每个线程一条记录：同一 pid 的多个线程要按 pid 求和 */
    struct kinfo_proc kp[3];

    memset(kp, 0, sizeof(kp));
    kp[0].ki_pid    = 97636;
    kp[0].ki_pctcpu = FSCALE;          /* 100% */
    kp[1].ki_pid    = 97636;
    kp[1].ki_pctcpu = FSCALE / 2;      /* 50%，同 pid 求和 → 150% */
    kp[2].ki_pid    = 4634;
    kp[2].ki_pctcpu = FSCALE / 4;      /* 25% */

    GHashTable *map = proc_ps_parse_kproc(kp, sizeof(kp));

    g_assert_cmpuint(g_hash_table_size(map), ==, 2);

    int k1 = 97636;
    int k2 = 4634;
    int k3 = 1;

    double *v1 = g_hash_table_lookup(map, &k1);
    double *v2 = g_hash_table_lookup(map, &k2);

    g_assert_nonnull(v1);
    g_assert_nonnull(v2);
    g_assert_cmpfloat_with_epsilon(*v1, 150.0, 0.001);
    g_assert_cmpfloat_with_epsilon(*v2, 25.0, 0.001);
    g_assert_null(g_hash_table_lookup(map, &k3));
    g_hash_table_destroy(map);

    /* 长度不是结构体整数倍时，尾部残片必须被忽略而不是越界：
     * 少 3 字节只剩前两条记录（同一 pid 的两个线程）。 */
    map = proc_ps_parse_kproc(kp, sizeof(kp) - 3);
    g_assert_cmpuint(g_hash_table_size(map), ==, 1);
    {
        double *only = g_hash_table_lookup(map, &k1);
        g_assert_nonnull(only);
        g_assert_cmpfloat_with_epsilon(*only, 150.0, 0.001);
    }
    g_hash_table_destroy(map);

    map = proc_ps_parse_kproc(NULL, 0);
    g_assert_cmpuint(g_hash_table_size(map), ==, 0);
    g_hash_table_destroy(map);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/process_ps/fields", test_parse_fields);
    g_test_add_func("/process_ps/fingerprint", test_fingerprint);
    g_test_add_func("/process_ps/category", test_category);
    g_test_add_func("/process_ps/null", test_null_input);
    g_test_add_func("/process_ps/kproc", test_kproc_aggregate);
    return g_test_run();
}
