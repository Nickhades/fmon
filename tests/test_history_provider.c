/*
 * test_history_provider.c - 历史 provider 解析/兼容测试
 *
 * 覆盖：17 列主层、16 列兼容、7 列旧格式、GPU sidecar、ZFS sidecar。
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "history/history_provider.h"

static char *s_dir;
static char *s_raw;

static char *write_sidecar(const char *suffix, const char *body)
{
    char *path = g_strdup_printf("%s%s", s_raw, suffix);

    g_assert_true(g_file_set_contents(path, body, -1, NULL));
    return path;
}

static void test_main_17_and_disk_temp(void)
{
    gint64 now = (gint64)time(NULL);
    GString *s = g_string_new(NULL);

    for (int i = 4; i >= 0; i--) {
        g_string_append_printf(
            s,
            "%lld\t20.0\t19.0\t21.0\t50.0\t49.0\t51.0\t1.0\t"
            "100.0\t50.0\t10.0\t20.0\t40.0\t39.0\t41.0\t45.5\t6\n",
            (long long)(now - i * 60));
    }

    write_sidecar("_1m.tsv", s->str);
    g_string_free(s, TRUE);

    g_assert_true(fmon_history_load(3600));
    g_assert_cmpuint(fmon_history_count(), >, 0);

    const FmonHistorySeries *dt =
        fmon_history_series(FMON_HIST_DISK_TEMP);
    g_assert_nonnull(dt);
    g_assert_true(dt->available);
    g_assert_cmpfloat(fabs(dt->values[0] - 45.5), <, 0.01);
}

static void test_main_16_no_disk_temp(void)
{
    gint64 now = (gint64)time(NULL);
    GString *s = g_string_new(NULL);

    for (int i = 1; i >= 0; i--) {
        g_string_append_printf(
            s,
            "%lld\t20.0\t19.0\t21.0\t50.0\t49.0\t51.0\t1.0\t"
            "100.0\t50.0\t10.0\t20.0\t40.0\t39.0\t41.0\t6\n",
            (long long)(now - i * 60));
    }

    write_sidecar("_1m.tsv", s->str);
    g_string_free(s, TRUE);

    g_assert_true(fmon_history_load(3600));
    const FmonHistorySeries *dt =
        fmon_history_series(FMON_HIST_DISK_TEMP);
    g_assert_nonnull(dt);
    g_assert_false(dt->available);
}

static void test_main_7_old_format(void)
{
    gint64 now = (gint64)time(NULL);
    GString *s = g_string_new(NULL);

    for (int i = 1; i >= 0; i--) {
        g_string_append_printf(s, "%lld\t20.0\t19.0\t21.0\t50.0\t49.0\t51.0\n",
                               (long long)(now - i * 60));
    }

    write_sidecar("_1m.tsv", s->str);
    g_string_free(s, TRUE);

    /* 旧 7 列 1m 只有 CPU/内存，provider 会回退 raw 10s 文件。 */
    GString *raw = g_string_new(NULL);

    for (int i = 1; i >= 0; i--)
        g_string_append_printf(
            raw, "%lld\t20.0\t50.0\t1.0\t100.0\t50.0\t10.0\t20.0\t40.0\n",
            (long long)(now - i * 60));

    g_assert_true(g_file_set_contents(s_raw, raw->str, -1, NULL));
    g_string_free(raw, TRUE);

    g_assert_true(fmon_history_load(3600));
    const FmonHistorySeries *cpu = fmon_history_series(FMON_HIST_CPU);
    g_assert_nonnull(cpu);
    g_assert_true(cpu->available);
}

static void test_gpu_sidecar(void)
{
    gint64 now = (gint64)time(NULL);
    GString *s = g_string_new(NULL);

    for (int i = 2; i >= 0; i--) {
        gint64 ts = now - i * 60;

        g_string_append_printf(s, "%lld\t0\t30.0\t45.0\t2048.0\n",
                               (long long)ts);
        g_string_append_printf(s, "%lld\t1\t50.0\t55.0\t4096.0\n",
                               (long long)ts);
    }

    write_sidecar("_gpu_1m.tsv", s->str);
    g_string_free(s, TRUE);

    g_assert_true(fmon_gpu_history_load(3600));
    g_assert_cmpuint(fmon_gpu_history_count(0), >, 0);
    g_assert_cmpuint(fmon_gpu_history_count(1), >, 0);

    const FmonHistorySeries *u =
        fmon_gpu_history_series(0, FMON_GPU_HIST_USAGE);
    g_assert_nonnull(u);
    g_assert_true(u->available);
    g_assert_cmpfloat(fabs(u->values[0] - 30.0), <, 0.01);
}

static void test_zfs_sidecar(void)
{
    gint64 now = (gint64)time(NULL);
    GString *s = g_string_new(NULL);

    for (int i = 2; i >= 0; i--) {
        g_string_append_printf(
            s, "%lld\t60.0\t90.0\t12.0\t80.0\t100.0\t50.0\n",
            (long long)(now - i * 60));
    }

    write_sidecar("_zfs_1m.tsv", s->str);
    g_string_free(s, TRUE);

    g_assert_true(fmon_zfs_history_load(3600));
    const FmonHistorySeries *arc =
        fmon_zfs_history_series(FMON_ZFS_HIST_ARC_PCT);
    g_assert_nonnull(arc);
    g_assert_true(arc->available);
    g_assert_cmpfloat(fabs(arc->values[0] - 90.0), <, 0.01);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    s_dir = g_dir_make_tmp("fmon-hist-XXXXXX", NULL);
    g_assert_nonnull(s_dir);
    s_raw = g_build_filename(s_dir, "history", NULL);
    g_setenv("FMON_HISTORY_FILE", s_raw, TRUE);

    g_test_add_func("/history/17-disk-temp", test_main_17_and_disk_temp);
    g_test_add_func("/history/16-no-disk-temp",
                    test_main_16_no_disk_temp);
    g_test_add_func("/history/7-old", test_main_7_old_format);
    g_test_add_func("/history/gpu-sidecar", test_gpu_sidecar);
    g_test_add_func("/history/zfs-sidecar", test_zfs_sidecar);

    int ret = g_test_run();

    g_free(s_raw);
    g_free(s_dir);
    return ret;
}
