/*
 * test_shm.c - 共享内存（seqlock）单元测试
 *
 * 覆盖：
 *   1. 创建后的头部字段（magic / ABI / struct_size / pid）；
 *   2. 写者与读者并发时，快照不会读到“写了一半”的数据（seqlock 的核心保证）；
 *   3. ABI 版本 / 结构体大小不匹配时，attach 必须拒绝而不是崩溃。
 *
 * 测试使用独立命名的共享内存对象，不会干扰正在运行的 fmon-daemon。
 */

#include <glib.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fmon_shm.h"

static char      g_name[64];
static FmonShmData *g_shm;
static gboolean  g_writer_done;

static void make_name(void)
{
    g_snprintf(g_name, sizeof(g_name), "/fmon_unit_test_%d", (int)getpid());
}

static void cleanup_object(void)
{
    shm_unlink(g_name);
}

/* --------------------------------------------------------------------------
 * 1. 创建后的头部
 * ----------------------------------------------------------------------- */
static void test_create_header(void)
{
    char err[256] = "";
    int  fd       = -1;

    FmonShmData *shm = fmon_shm_create_for_test(g_name, &fd, err, sizeof(err));

    g_assert_nonnull(shm);
    g_assert_cmpuint(shm->magic, ==, FMON_SHM_MAGIC);
    g_assert_cmpuint(shm->abi_version, ==, FMON_ABI_VERSION);
    g_assert_cmpuint(shm->struct_size, ==, sizeof(FmonShmData));
    g_assert_cmpint(shm->daemon_pid, ==, (int32_t)getpid());

    fmon_shm_detach(shm);
    if (fd >= 0)
        close(fd);
    cleanup_object();
}

/* --------------------------------------------------------------------------
 * 2. 并发快照一致性：写者把两个字段写成同一个值，读者断言两者相等
 * ----------------------------------------------------------------------- */
static gpointer writer_thread(gpointer data)
{
    (void)data;

    for (int i = 0; i < 50000; i++) {
        fmon_shm_wr_begin();
        g_shm->cpu_usage_pct     = (double)i;
        g_shm->mem_used_pct      = (double)i;
        g_shm->history_index     = (uint32_t)i;
        fmon_shm_wr_end();
    }

    g_writer_done = TRUE;
    return NULL;
}

static void test_snapshot_consistency(void)
{
    char err[256] = "";
    int  fd       = -1;

    g_shm = fmon_shm_create_for_test(g_name, &fd, err, sizeof(err));
    g_assert_nonnull(g_shm);

    fmon_shm_writer_init(g_shm);

    g_writer_done = FALSE;

    pthread_t th;
    g_assert_cmpint(pthread_create(&th, NULL, writer_thread, NULL), ==, 0);

    guint reads = 0;
    guint busy  = 0;

    while (!g_writer_done || reads < 1000) {
        FmonShmData snap;

        /* 写者可能正好在事务中，snapshot 会重试 64 次后返回 -1；
         * 这属于正常的“忙”状态，不是错误。 */
        if (fmon_shm_snapshot(g_shm, &snap) != 0) {
            busy++;
            continue;
        }

        /* 同一个写临界区里写入的两个字段，快照里必须相等 */
        g_assert_cmpfloat(snap.cpu_usage_pct, ==, snap.mem_used_pct);
        g_assert_cmpuint(snap.abi_version, ==, FMON_ABI_VERSION);

        reads++;
    }

    pthread_join(th, NULL);

    g_assert_cmpuint(reads, >=, 1000);
    g_test_message("读取成功 %u 次，写者繁忙 %u 次", reads, busy);

    fmon_shm_detach(g_shm);
    if (fd >= 0)
        close(fd);
    cleanup_object();
}

/* --------------------------------------------------------------------------
 * 3. 头部损坏时 attach 必须拒绝
 * ----------------------------------------------------------------------- */
static void test_attach_rejects_bad_header(void)
{
    char err[256] = "";
    int  fd       = -1;

    /* 先创建一个合法的对象，然后脱离映射（对象仍在） */
    FmonShmData *shm = fmon_shm_create_for_test(g_name, &fd, err, sizeof(err));
    g_assert_nonnull(shm);
    fmon_shm_detach(shm);
    if (fd >= 0)
        close(fd);

    /* 正常附加应成功 */
    FmonShmData *ok = fmon_shm_attach_for_test(g_name, TRUE, err, sizeof(err));
    g_assert_nonnull(ok);

    /* 篡改 ABI 版本 */
    ok->abi_version = 999;
    fmon_shm_detach(ok);

    err[0] = '\0';
    FmonShmData *bad = fmon_shm_attach_for_test(g_name, TRUE, err, sizeof(err));
    g_assert_null(bad);
    g_assert_nonnull(strstr(err, "ABI"));

    cleanup_object();
}

static void test_attach_rejects_bad_size(void)
{
    char err[256] = "";
    int  fd       = -1;

    FmonShmData *shm = fmon_shm_create_for_test(g_name, &fd, err, sizeof(err));
    g_assert_nonnull(shm);

    fmon_shm_detach(shm);
    if (fd >= 0)
        close(fd);

    /* 把对象截短，模拟“守护进程与界面结构体大小不一致” */
    int tfd = shm_open(g_name, O_RDWR, 0);
    g_assert_cmpint(tfd, >=, 0);
    g_assert_cmpint(ftruncate(tfd, 128), ==, 0);
    close(tfd);

    err[0] = '\0';
    FmonShmData *bad = fmon_shm_attach_for_test(g_name, TRUE, err, sizeof(err));
    g_assert_null(bad);
    g_assert_nonnull(strstr(err, "大小不匹配"));

    cleanup_object();
}

int main(int argc, char **argv)
{
    make_name();

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/shm/create_header", test_create_header);
    g_test_add_func("/shm/snapshot_consistency", test_snapshot_consistency);
    g_test_add_func("/shm/reject_bad_abi", test_attach_rejects_bad_header);
    g_test_add_func("/shm/reject_bad_size", test_attach_rejects_bad_size);

    return g_test_run();
}
