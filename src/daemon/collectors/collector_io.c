/*
 * collector_io.c - 磁盘吞吐（devstat）
 *
 * 旧版本用
 *     popen("iostat -d -K 1 2 | awk '...'")
 * 每 100ms 执行一次：既要 fork 两个进程 + awk，iostat 自己还要 sleep 1 秒。
 * 结果是采集线程几乎永远在阻塞，CPU 占用高，而且 awk 把所有分区列都加在
 * 一起，数值会被重复计算。
 *
 * 现在改为直接使用 FreeBSD 的 devstat(3) API（链接 -ldevstat）：
 *   1. 不创建任何子进程；
 *   2. 只统计整盘设备（跳过 DEVSTAT_TYPE_PASS 直通设备），不会重复计数；
 *   3. 速率用“本次累计 - 上次累计”除以真实经过的时间，结果准确。
 *
 * GPU 采集已拆分到 collector_gpu.c（支持多显卡自动探测与切换）。
 */

#include "fmon_daemon.h"

#include "disk_smart.h"
#include "disk_meta.h"
#include "collector_io_meta.h"
#include "fmon_child.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include "fmon_sysctl.h"

#include <devstat.h>
#include <stdlib.h>

/* libdevstat 的内部缓冲区由 devstat_getdevs() 分配，需要调用者释放。
 * 放在文件作用域，线程退出时统一清理。 */
static struct statinfo g_devstat;

static void devstat_release(void)
{
    if (g_devstat.dinfo == NULL)
        return;

    free(g_devstat.dinfo->mem_ptr);
    free(g_devstat.dinfo);
    g_devstat.dinfo = NULL;
}

/* ===========================================================================
 * 磁盘
 * ========================================================================= */

/* devstat 上报的是驱动名（如 nda），/dev 下是带单元号的节点（nda0）。
 * 用 kern.disks 把它们对应起来。 */
static void map_disk_name(const char *devstat_name, char *out, size_t out_len)
{
    size_t len = 0;
    char  *disks = NULL;

    if (sysctlbyname("kern.disks", NULL, &len, NULL, 0) == 0 && len > 0) {
        disks = malloc(len);

        if (disks != NULL &&
            sysctlbyname("kern.disks", disks, &len, NULL, 0) == 0) {
            char  *save = NULL;
            size_t plen = strlen(devstat_name);

            for (char *tok = strtok_r(disks, " ", &save); tok != NULL;
                 tok = strtok_r(NULL, " ", &save)) {
                if (strncmp(tok, devstat_name, plen) == 0 &&
                    isdigit((unsigned char)tok[plen])) {
                    strlcpy(out, tok, out_len);
                    free(disks);
                    return;
                }
            }
        }

        free(disks);
    }

    snprintf(out, out_len, "%s0", devstat_name);
}

static void sample_disk(FmonShmData *s)
{
    static bool     primed  = false;
    static int64_t  prev_ms = 0;
    static uint64_t prev_r  = 0, prev_w = 0;

    /* 每块磁盘上一次的累计值（按设备名匹配，防止设备顺序变化） */
    static struct {
        char     name[16];
        uint64_t rb, wb;
        double   busy_us;
        int64_t  ms;
    } dprev[FMON_MAX_DISKS];

    if (g_devstat.dinfo == NULL) {
        g_devstat.dinfo = calloc(1, sizeof(*g_devstat.dinfo));
        if (g_devstat.dinfo == NULL)
            return;
    }

    if (devstat_getdevs(NULL, &g_devstat) < 0 || g_devstat.dinfo->numdevs <= 0)
        return;

    int64_t now = fmon_now_ms();

    uint64_t r = 0, w = 0;
    uint64_t ops_r = 0, ops_w = 0;
    int      n = 0;

    for (int i = 0; i < g_devstat.dinfo->numdevs; i++) {
        const struct devstat *d = &g_devstat.dinfo->devices[i];

        /* PASS 设备是原始直通设备，和整盘设备统计的是同一份 I/O */
        if ((d->device_type & DEVSTAT_TYPE_PASS) != 0)
            continue;

        uint64_t rb = d->bytes[DEVSTAT_READ];
        uint64_t wb = d->bytes[DEVSTAT_WRITE];

        r     += rb;
        w     += wb;
        ops_r += d->operations[DEVSTAT_READ];
        ops_w += d->operations[DEVSTAT_WRITE];

        /* 每块磁盘的明细（最多 FMON_MAX_DISKS 块） */
        if (n < FMON_MAX_DISKS) {
            FmonDiskInfo *di = &s->disks[n];

            memset(di, 0, sizeof(*di));

            char mapped[32];

            map_disk_name(d->device_name, mapped, sizeof(mapped));
            strlcpy(di->name, mapped, sizeof(di->name));

            /* 同名设备累计字节突然变小：说明换了盘，重新探测型号/类型 */
            if (dprev[n].ms > 0 &&
                (rb < dprev[n].rb || wb < dprev[n].wb))
                disk_io_meta_invalidate(mapped);

            disk_io_fill_meta(di, now);

            FmonSmartInfo smart;
            uint32_t      smart_source = FMON_SMART_SRC_NONE;

            fmon_disk_smart_probe(di->name, now, &smart, &smart_source);
            di->temp_c      = smart.valid ? smart.temp_c : -1.0;
            di->spare_pct   = smart.valid ? smart.spare_pct : -1;
            di->wear_pct    = smart.valid ? smart.wear_pct : -1;
            di->smart_valid = smart.valid ? 1u : 0u;
            di->smart_source = smart_source;

            di->read_bytes  = rb;
            di->write_bytes = wb;
            di->read_ops    = d->operations[DEVSTAT_READ];
            di->write_ops   = d->operations[DEVSTAT_WRITE];

            if (strcmp(dprev[n].name, d->device_name) != 0) {
                strlcpy(dprev[n].name, d->device_name, sizeof(dprev[n].name));
                dprev[n].ms = 0;
            }

            if (dprev[n].ms > 0 && now > dprev[n].ms) {
                double dt = (double)(now - dprev[n].ms) / 1000.0;

                if (dt > 0.05) {
                    if (rb >= dprev[n].rb)
                        di->read_kbps =
                            (double)(rb - dprev[n].rb) / 1024.0 / dt;
                    if (wb >= dprev[n].wb)
                        di->write_kbps =
                            (double)(wb - dprev[n].wb) / 1024.0 / dt;
                    if (d->busy_time.sec >= 0) {
                        double busy_us =
                            (double)d->busy_time.sec * 1000000.0 +
                            (double)d->busy_time.frac * 1000000.0 / 4294967296.0;

                        if (busy_us >= dprev[n].busy_us)
                            di->busy_pct =
                                100.0 * (busy_us - dprev[n].busy_us) /
                                1000000.0 / dt;
                        if (di->busy_pct > 100.0)
                            di->busy_pct = 100.0;
                    }
                }
            }

            dprev[n].rb   = rb;
            dprev[n].wb   = wb;
            dprev[n].busy_us =
                (double)d->busy_time.sec * 1000000.0 +
                (double)d->busy_time.frac * 1000000.0 / 4294967296.0;
            dprev[n].ms   = now;
            n++;
        }
    }

    s->disk_count = (uint32_t)n;

    if (primed && now > prev_ms) {
        double dt = (double)(now - prev_ms) / 1000.0;
        if (dt > 0.05) {
            if (r >= prev_r)
                s->disk_read_kbps = (double)(r - prev_r) / 1024.0 / dt;
            if (w >= prev_w)
                s->disk_write_kbps = (double)(w - prev_w) / 1024.0 / dt;
        }
    }

    s->disk_read_ops  = ops_r;
    s->disk_write_ops = ops_w;

    prev_r  = r;
    prev_w  = w;
    prev_ms = now;
    primed  = true;
}

/* ===========================================================================
 * 线程主循环
 * ========================================================================= */
void *thread_io(void *arg)
{
    (void)arg;

    int tick = 0;

    while (g_running) {
        /* devstat 是纯 sysctl 读取，微秒级，放在写事务里没有问题 */
        fmon_shm_wr_begin();
        sample_disk(g_shm);
        fmon_shm_wr_end();

        /* GPU 探测较重（可能要 fork nvidia-smi，数百毫秒），
         * 由 fmon_gpu_collect() 自己管理写锁，避免阻塞发布线程的心跳。 */
        if ((tick % 5) == 0)
            fmon_gpu_collect(g_shm);

        tick++;

        if (!fmon_sleep_ms(FMON_SAMPLE_INTERVAL_MS) && !g_running)
            break;
    }

    devstat_release();
    return NULL;
}
