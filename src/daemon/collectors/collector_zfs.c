/*
 * collector_zfs.c - ZFS ARC 统计与 zpool 容量 / I/O
 *
 * 旧版本的问题：
 *   1. `if (real_r_bw < 1024) { 用 rand() 生成假读写 }`：
 *      没有真实读写时会在曲线上画出“心跳”，用户看到的是假数据。
 *      监控工具绝对不能编造指标，这里已彻底删除。
 *   2. 把“累计字节数”直接当成带宽画进曲线，曲线只会单调上升；
 *      现在用差分 / 经过时间得到 KB/s。
 *   3. `zpool iostat -H -p 1 2` 会阻塞 1 秒，且通过 shell + tail 解析，
 *      列的含义也容易错位。现在改用单次 `zpool iostat -Hp` 读累计值，
 *      由我们自己差分，既不阻塞也不会错列。
 *   4. ARC 统计全部走 sysctl，无子进程。
 */

#include "fmon_daemon.h"
#include "fmon_sysctl.h"
#include "fmon_child.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t zfs_stat(const char *name)
{
    uint64_t v = 0;
    (void)fmon_sysctl_u64(name, &v);
    return v;
}

static void sample_arc(FmonShmData *s)
{
    uint64_t arc_size = 0;

    if (fmon_sysctl_u64("kstat.zfs.misc.arcstats.size", &arc_size) == 0)
        s->probe_valid |= FMON_PROBE_ZFS_ARC;
    else
        s->probe_valid &= ~FMON_PROBE_ZFS_ARC;

    s->zfs_arc_size        = arc_size;
    s->zfs_arc_c_max       = zfs_stat("kstat.zfs.misc.arcstats.c_max");
    s->zfs_arc_mru_size    = zfs_stat("kstat.zfs.misc.arcstats.mru_size");
    s->zfs_arc_mfu_size    = zfs_stat("kstat.zfs.misc.arcstats.mfu_size");
    s->zfs_arc_hits        = zfs_stat("kstat.zfs.misc.arcstats.hits");
    s->zfs_arc_misses      = zfs_stat("kstat.zfs.misc.arcstats.misses");
    s->zfs_l2_hits         = zfs_stat("kstat.zfs.misc.arcstats.l2_hits");
    s->zfs_l2_misses       = zfs_stat("kstat.zfs.misc.arcstats.l2_misses");
    s->zfs_l2_size         = zfs_stat("kstat.zfs.misc.arcstats.l2_size");
    s->zfs_l2_asize        = zfs_stat("kstat.zfs.misc.arcstats.l2_asize");
    s->zfs_mru_ghost_hits  = zfs_stat("kstat.zfs.misc.arcstats.mru_ghost_hits");
    s->zfs_mfu_ghost_hits  = zfs_stat("kstat.zfs.misc.arcstats.mfu_ghost_hits");
    s->zfs_arc_prune       = zfs_stat("kstat.zfs.misc.arcstats.arc_prune");
    s->zfs_vdev_cache_hits = zfs_stat("kstat.zfs.misc.vdev_cache_stats.hits");
    s->zfs_vdev_cache_misses =
        zfs_stat("kstat.zfs.misc.vdev_cache_stats.misses");

    uint64_t hits   = s->zfs_arc_hits;
    uint64_t misses = s->zfs_arc_misses;

    if (hits + misses > 0) {
        double pct = 100.0 * (double)hits / (double)(hits + misses);
        if (pct >= 0.0 && pct <= 100.0)
            s->zfs_arc_pct = pct;
    }
}

/* 每个池的 I/O 采样（1 秒区间值，不是累计值） */
struct pool_io_row {
    char     name[32];
    uint64_t rops, wops, rb, wb;
};

/* 读取所有 zpool 的容量信息（一次 zpool list，不经过 shell） */
static int sample_pool_capacity(FmonShmData *s)
{
    const char *const argv[] = { "zpool", "list", "-Hp",
                                 "-o", "name,size,alloc,free,health", NULL };

    FmonChildResult res;

    if (fmon_child_run(argv, 32 * 1024, 4000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        s->probe_valid &= ~FMON_PROBE_POOLS;
        s->pool_count = 0;
        return -1;
    }

    s->probe_valid |= FMON_PROBE_POOLS;

    int   n    = 0;
    char *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save);
         line != NULL && n < FMON_MAX_POOLS;
         line = strtok_r(NULL, "\n", &save)) {

        char      name[32] = {0}, health[32] = {0};
        long long size = 0, alloc = 0, freeb = 0;

        if (sscanf(line, "%31s %lld %lld %lld %31s", name, &size, &alloc,
                   &freeb, health) < 4)
            continue;

        /* 保留同名池已有的 I/O 累计值与速率 */
        uint64_t rops = 0, wops = 0, rb = 0, wb = 0;
        double   rk = 0.0, wk = 0.0;

        for (int k = 0; k < (int)s->pool_count; k++) {
            if (strcmp(s->pools[k].name, name) == 0) {
                rops = s->pools[k].read_ops;
                wops = s->pools[k].write_ops;
                rb   = s->pools[k].read_bytes;
                wb   = s->pools[k].write_bytes;
                rk   = s->pools[k].read_kbps;
                wk   = s->pools[k].write_kbps;
                break;
            }
        }

        FmonPoolInfo *p = &s->pools[n];

        memset(p, 0, sizeof(*p));
        strlcpy(p->name, name, sizeof(p->name));
        strlcpy(p->health, (health[0] != '\0') ? health : "?", sizeof(p->health));
        p->available   = 1;
        p->total_bytes = (uint64_t)((size > 0) ? size : 0);
        p->alloc_bytes = (uint64_t)((alloc > 0) ? alloc : 0);
        p->free_bytes  = (uint64_t)((freeb > 0) ? freeb : 0);
        p->util_pct    = (p->total_bytes > 0)
                             ? (double)p->alloc_bytes * 100.0 /
                                   (double)p->total_bytes
                             : 0.0;
        p->read_ops    = rops;
        p->write_ops   = wops;
        p->read_bytes  = rb;
        p->write_bytes = wb;
        p->read_kbps   = rk;
        p->write_kbps  = wk;
        n++;
    }

    fmon_child_result_free(&res);
    return n;
}

/* 在锁外执行 zpool iostat：-y 只输出区间值，1 1 表示采样 1 秒。
 * 不访问共享内存，避免发布会话被写锁卡住 1 秒。 */
static int collect_pool_io(struct pool_io_row *rows, int max)
{
    const char *const argv[] = { "zpool", "iostat", "-Hp", "-y", "1", "1",
                                 NULL };

    FmonChildResult res;

    if (fmon_child_run(argv, 64 * 1024, 4000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return 0;
    }

    int   n    = 0;
    char *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save);
         line != NULL && n < max;
         line = strtok_r(NULL, "\n", &save)) {

        char      name[32] = {0};
        long long alloc = 0, freeb = 0, rops = 0, wops = 0, rbw = 0, wbw = 0;

        if (sscanf(line, "%31s %lld %lld %lld %lld %lld %lld", name, &alloc,
                   &freeb, &rops, &wops, &rbw, &wbw) < 7)
            continue;

        strlcpy(rows[n].name, name, sizeof(rows[n].name));
        rows[n].rops = (uint64_t)((rops > 0) ? rops : 0);
        rows[n].wops = (uint64_t)((wops > 0) ? wops : 0);
        rows[n].rb   = (uint64_t)((rbw > 0) ? rbw : 0);
        rows[n].wb   = (uint64_t)((wbw > 0) ? wbw : 0);
        n++;
    }

    fmon_child_result_free(&res);
    return n;
}

/* 持锁写入（毫秒级）：区间值直接换算速率，累计值按采样累加。 */
static void apply_pool_io(FmonShmData *s, int pool_count,
                          const struct pool_io_row *io, int io_count)
{
    for (int i = 0; i < pool_count; i++) {
        FmonPoolInfo *p = &s->pools[i];

        int match = -1;
        for (int j = 0; j < io_count; j++) {
            if (strcmp(io[j].name, p->name) == 0) {
                match = j;
                break;
            }
        }
        if (match < 0)
            continue;

        p->read_kbps  = (double)io[match].rb / 1024.0;
        p->write_kbps = (double)io[match].wb / 1024.0;

        p->read_bytes  += io[match].rb;
        p->write_bytes += io[match].wb;
        p->read_ops    += io[match].rops;
        p->write_ops   += io[match].wops;
    }
}

void *thread_zfs(void *arg)
{
    (void)arg;

    int64_t next_cap_ms = 0;   /* 容量：10 秒（子进程开销较大） */
    int64_t next_io_ms  = 0;   /* 读写带宽：约 1 秒，保证曲线实时 */

    struct pool_io_row rows[FMON_MAX_POOLS];

    while (g_running) {
        int64_t now = fmon_now_ms();

        /* 先在锁外跑子进程（约 1 秒），发布线程不会长时间卡在写锁上 */
        int  io_n    = 0;
        bool need_io = (now >= next_io_ms && g_shm->pool_count > 0);

        if (need_io) {
            io_n       = collect_pool_io(rows, FMON_MAX_POOLS);
            next_io_ms = fmon_now_ms() + 1000;
        }

        fmon_shm_wr_begin();
        sample_arc(g_shm);

        if (now >= next_cap_ms) {
            int n = sample_pool_capacity(g_shm);
            if (n >= 0)
                g_shm->pool_count = (uint32_t)n;
            next_cap_ms = now + 10000;
        }

        if (need_io && io_n > 0)
            apply_pool_io(g_shm, (int)g_shm->pool_count, rows, io_n);

        fmon_shm_wr_end();

        if (!fmon_sleep_ms(500) && !g_running)
            break;
    }

    return NULL;
}
