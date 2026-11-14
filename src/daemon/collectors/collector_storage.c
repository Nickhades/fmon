/*
 * collector_storage.c - 文件系统挂载点与 Swap
 *
 * 旧版本完全没有“存储空间”信息，资源管理器里看不到哪个分区快满了。
 * 这里通过 getmntinfo() + statvfs() 采集每个真实文件系统的容量，
 * 通过 swapinfo(8) 采集交换区使用情况（子进程执行，但每 5 秒一次、
 * 不经过 shell、带 2 秒超时）。
 */

#include "fmon_daemon.h"
#include "fmon_sysctl.h"
#include "fmon_child.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* 这些是内核伪文件系统，没有“容量”概念，列出来只会干扰用户 */
static bool is_pseudo_fs(const char *type)
{
    static const char *const skip[] = {
        "devfs", "fdescfs", "procfs", "linprocfs", "linsysfs",
        "autofs", "nullfs", "unionfs", "pseudofs", "fusefs.portal"
    };

    for (size_t i = 0; i < FMON_ARRAY_LEN(skip); i++) {
        if (strcmp(type, skip[i]) == 0)
            return true;
    }
    return false;
}

static int cmp_mount(const void *a, const void *b)
{
    const FmonMountInfo *ma = a;
    const FmonMountInfo *mb = b;
    return strcmp(ma->mountpoint, mb->mountpoint);
}

static void sample_mounts(FmonShmData *s)
{
    struct statfs *mnt = NULL;
    int            n   = getmntinfo(&mnt, MNT_NOWAIT);

    if (n <= 0) {
        s->mount_count = 0;
        return;
    }

    FmonMountInfo tmp[FMON_MAX_MOUNTS * 4];
    int           cnt = 0;

    memset(tmp, 0, sizeof(tmp));

    for (int i = 0; i < n && cnt < (int)FMON_ARRAY_LEN(tmp); i++) {
        const char *fstype = mnt[i].f_fstypename;

        if (is_pseudo_fs(fstype))
            continue;
        if (mnt[i].f_mntonname[0] == '\0')
            continue;

        struct statvfs vfs;
        if (statvfs(mnt[i].f_mntonname, &vfs) != 0)
            continue;
        if (vfs.f_blocks == 0)
            continue;

        uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        uint64_t freeb = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        uint64_t used  = (total > freeb) ? total - freeb : 0;

        FmonMountInfo *m = &tmp[cnt++];
        strlcpy(m->mountpoint, mnt[i].f_mntonname, sizeof(m->mountpoint));
        strlcpy(m->device, mnt[i].f_mntfromname, sizeof(m->device));
        strlcpy(m->fstype, fstype, sizeof(m->fstype));
        m->total_bytes = total;
        m->free_bytes  = avail;

        /* 与 df(1) 的 Use% 保持一致：used / (used + avail) */
        if (used + avail > 0)
            m->used_pct = 100.0 * (double)used / (double)(used + avail);
        else
            m->used_pct = 0.0;
    }

    qsort(tmp, (size_t)cnt, sizeof(tmp[0]), cmp_mount);

    if (cnt > FMON_MAX_MOUNTS)
        cnt = FMON_MAX_MOUNTS;

    s->mount_count = (uint32_t)cnt;
    memcpy(s->mounts, tmp, sizeof(tmp[0]) * (size_t)cnt);
}

static void sample_swap(FmonShmData *s)
{
    const char *const argv[] = { "swapinfo", "-k", NULL };

    FmonChildResult res;
    if (fmon_child_run(argv, 256 * 1024, 2000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return;
    }

    uint64_t total_kb = 0, used_kb = 0;
    char    *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char      dev[64] = {0};
        long long t = 0, u = 0;

        /* 表头 "Device 1K-blocks Used ..." 会因为 %lld 解析失败被跳过 */
        if (sscanf(line, "%63s %lld %lld", dev, &t, &u) != 3)
            continue;
        if (t <= 0)
            continue;

        total_kb += (uint64_t)t;
        used_kb  += (uint64_t)((u > 0) ? u : 0);
    }

    s->swap_total_kb = total_kb;
    s->swap_used_kb  = used_kb;
    s->swap_used_pct = (total_kb > 0)
                           ? 100.0 * (double)used_kb / (double)total_kb
                           : 0.0;

    fmon_child_result_free(&res);
}

void *thread_storage(void *arg)
{
    (void)arg;

    while (g_running) {
        fmon_shm_wr_begin();
        sample_mounts(g_shm);
        sample_swap(g_shm);
        fmon_shm_wr_end();

        /* 容量变化很慢，5 秒一次足够，也减少 swapinfo 子进程开销 */
        for (int i = 0; i < 5 && g_running; i++) {
            if (!fmon_sleep_ms(1000) && !g_running)
                break;
        }
    }

    return NULL;
}
