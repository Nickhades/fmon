/*
 * fmon_daemon.c - fmon 监控守护进程
 *
 * 结构：
 *   main()
 *     ├── 创建/校验共享内存（含单实例锁、权限收紧）
 *     ├── 写入静态系统信息（主机名、CPU 型号、启动时间……）
 *     ├── 启动 6 个采集线程 + 1 个发布线程
 *     ├── 主线程只负责等待信号
 *     └── 收到 SIGINT/SIGTERM → 通知线程退出 → join → 标记 STOPPED → 解除映射
 *
 * 并发模型：
 *   采集线程各自负责不同字段，写入时通过 fmon_shm_wr_begin()/end() 进入
 *   seqlock 写事务；发布线程是唯一推进 history_index 的线程，保证历史曲线
 *   每个采样点只写入一次（旧版本在每个采集线程里都推进 index，导致曲线
 *   只有一半数据）。
 *
 * 注意：本程序必须由 root 运行（kvm、部分 sysctl、/var/run 下的 shm）。
 * 界面本身不需要 root，只以只读方式映射共享内存。
 */

#include "fmon_daemon.h"
#include "fmon_ctl.h"
#include "history_store.h"
#include "fmon_sysctl.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

FmonShmData *g_shm = NULL;
volatile sig_atomic_t g_running = 1;

static int  g_shm_fd = -1;
static bool g_use_syslog = false;

/* 记录系统启动时间，用于计算 uptime */
static time_t g_boot_time = 0;

static void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void dlog(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[FMON-DAEMON] %s\n", buf);
    if (g_use_syslog)
        syslog(LOG_INFO, "%s", buf);
}

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* rc.d / daemon(8) 启动时 PATH 往往只有 /sbin:/bin:/usr/sbin:/usr/bin，
 * 而 nvidia-smi 等工具在安装前缀的 bin 下。这里按前缀补全，
 * 保证所有子进程可用（不写死 /usr/local）。 */
static void ensure_path(void)
{
    char extra[512];

    snprintf(extra, sizeof(extra), "%s/bin:%s/sbin",
             FMON_LOCALBASE, FMON_LOCALBASE);

    const char *cur = getenv("PATH");
    char        buf[1024];

    if (cur == NULL || cur[0] == '\0') {
        snprintf(buf, sizeof(buf), "%s:/usr/bin:/bin:/usr/sbin:/sbin", extra);
        (void)setenv("PATH", buf, 1);
        return;
    }
    if (strstr(cur, extra) != NULL)
        return;

    snprintf(buf, sizeof(buf), "%s:%s", extra, cur);
    (void)setenv("PATH", buf, 1);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* 不设 SA_RESTART：让 nanosleep 立刻返回 */

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);

    /* 写管道时不能因为对端关闭而杀死守护进程 */
    (void)signal(SIGPIPE, SIG_IGN);
}

static void fill_static_info(FmonShmData *shm)
{
    uint64_t ncpu = 0;
    struct timeval tv;
    size_t len;

    if (fmon_sysctl_str("kern.hostname", shm->hostname,
                        sizeof(shm->hostname)) != 0) {
        strlcpy(shm->hostname, "unknown", sizeof(shm->hostname));
    }

    if (fmon_sysctl_str("kern.osrelease", shm->sys_version,
                        sizeof(shm->sys_version)) != 0) {
        strlcpy(shm->sys_version, "FreeBSD", sizeof(shm->sys_version));
    }

    if (fmon_sysctl_str("hw.model", shm->cpu_model,
                        sizeof(shm->cpu_model)) != 0) {
        strlcpy(shm->cpu_model, "Unknown CPU", sizeof(shm->cpu_model));
    }

    if (fmon_sysctl_u64("hw.ncpu", &ncpu) == 0 && ncpu > 0 &&
        ncpu <= UINT16_MAX) {
        shm->cpu_count = (uint16_t)ncpu;
    } else {
        shm->cpu_count = 1;
    }

    len = sizeof(tv);
    if (sysctlbyname("kern.boottime", &tv, &len, NULL, 0) == 0 &&
        tv.tv_sec > 0) {
        g_boot_time = tv.tv_sec;
        time_t now = time(NULL);
        shm->uptime_sec = (now > g_boot_time) ? (uint64_t)(now - g_boot_time) : 0;
    }
}

static bool start_thread(const char *name, void *(*fn)(void *), pthread_t *tid)
{
    int rc = pthread_create(tid, NULL, fn, NULL);

    if (rc != 0) {
        dlog("启动线程 %s 失败: %s", name, strerror(rc));
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * 发布线程：唯一推进历史曲线与心跳的地方
 * ------------------------------------------------------------------------- */
void *thread_publisher(void *arg)
{
    (void)arg;

    int64_t next_ms = fmon_now_ms();

    while (g_running) {
        next_ms += FMON_SAMPLE_INTERVAL_MS;

        fmon_shm_wr_begin();

        FmonShmData *s = g_shm;
        uint32_t idx = s->history_index % FMON_HISTORY_POINTS;
        uint64_t sample_time_us = fmon_now_us();

        s->history_interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
        s->history_time_us[idx] = sample_time_us;

        /* 历史数组的语义：NaN = 该秒没有有效采样。
         * 不能用 0 代替：0% 和“没有传感器”在监控里是完全不同的结论。
         * 绘图端遇到 NaN 会断开折线，导出 CSV 时留空。 */
        s->cpu_history[idx] =
            isfinite(s->cpu_usage_pct) ? s->cpu_usage_pct : NAN;
        s->mem_history[idx] =
            isfinite(s->mem_used_pct) ? s->mem_used_pct : NAN;
        s->swap_history[idx] =
            ((s->probe_valid & FMON_PROBE_SWAP) != 0 &&
             isfinite(s->swap_used_pct))
                ? s->swap_used_pct
                : NAN;
        s->net_rx_history[idx] =
            isfinite(s->net_rx_kbps) ? s->net_rx_kbps : NAN;
        s->net_tx_history[idx] =
            isfinite(s->net_tx_kbps) ? s->net_tx_kbps : NAN;
        s->disk_read_history[idx] =
            isfinite(s->disk_read_kbps) ? s->disk_read_kbps : NAN;
        s->disk_write_history[idx] =
            isfinite(s->disk_write_kbps) ? s->disk_write_kbps : NAN;
        s->temp_history[idx] =
            isfinite(s->cpu_temp_c) ? s->cpu_temp_c : NAN;
        s->battery_history[idx] =
            ((s->probe_valid & FMON_PROBE_BATTERY) != 0 &&
             s->battery_present != 0 && isfinite(s->battery_pct) &&
             s->battery_pct >= 0.0)
                ? s->battery_pct
                : NAN;
        s->zpool_util_history[idx] =
            ((s->probe_valid & FMON_PROBE_POOLS) != 0 &&
             s->pool_count > 0 && s->pools[0].available != 0 &&
             isfinite(s->pools[0].util_pct))
                ? s->pools[0].util_pct
                : NAN;
        /* 曲线语义：ARC 大小占 ARC 上限的百分比，和界面“占上限”一致 */
        s->zfs_arc_history[idx] =
            ((s->probe_valid & FMON_PROBE_ZFS_ARC) != 0 &&
             s->zfs_arc_c_max > 0)
                ? 100.0 * (double)s->zfs_arc_size / (double)s->zfs_arc_c_max
                : NAN;
        s->zfs_hit_history[idx] =
            isfinite(s->zfs_arc_pct) ? s->zfs_arc_pct : NAN;

        /* 多池时历史曲线取所有池的合计带宽；没有池就是“无数据”。 */
        double pool_rd = 0.0, pool_wr = 0.0;

        for (uint32_t i = 0; i < s->pool_count && i < FMON_MAX_POOLS; i++) {
            if (isfinite(s->pools[i].read_kbps))
                pool_rd += s->pools[i].read_kbps;
            if (isfinite(s->pools[i].write_kbps))
                pool_wr += s->pools[i].write_kbps;
        }
        s->zfs_io_read_history[idx] =
            ((s->probe_valid & FMON_PROBE_POOLS) != 0 && s->pool_count > 0)
                ? pool_rd
                : NAN;
        s->zfs_io_write_history[idx] =
            ((s->probe_valid & FMON_PROBE_POOLS) != 0 && s->pool_count > 0)
                ? pool_wr
                : NAN;
        s->zfs_arc_prune_history[idx] = (double)s->zfs_arc_prune;

        /* L2ARC：未配置（size==0）时没有采样，写 NaN 让曲线保持空白。 */
        s->zfs_l2_size_history[idx] =
            (s->zfs_l2_size > 0)
                ? (double)s->zfs_l2_size / (1024.0 * 1024.0 * 1024.0)
                : NAN;

        uint64_t l2h = s->zfs_l2_hits;
        uint64_t l2m = s->zfs_l2_misses;

        s->zfs_l2_hit_history[idx] =
            (s->zfs_l2_size > 0 && l2h + l2m > 0)
                ? 100.0 * (double)l2h / (double)(l2h + l2m)
                : NAN;

        for (int g = 0; g < s->gpu_count && g < FMON_MAX_GPUS; g++) {
            /* 没有负载传感器的显卡写 NaN，不能写 0（会被误读成“一直空闲”）。 */
            s->gpu_history[g][idx] =
                (s->gpus[g].load_available != 0 &&
                 isfinite(s->gpus[g].usage_pct))
                    ? s->gpus[g].usage_pct
                    : NAN;
        }

        s->history_index = (idx + 1u) % FMON_HISTORY_POINTS;
        if (s->history_count < FMON_HISTORY_POINTS)
            s->history_count++;

        /* 更新 uptime（每 10 秒一次即可，避免频繁 sysctl） */
        if ((s->history_count % 10u) == 0u && g_boot_time > 0) {
            time_t now = time(NULL);
            if (now > g_boot_time)
                s->uptime_sec = (uint64_t)(now - g_boot_time);
        }

        s->last_update_us = fmon_now_us();
        s->status = (int32_t)FMON_STATUS_RUNNING;

        /* 每 10 秒把关键指标落盘，供界面查看 1 小时 / 24 小时历史 */
        {
            static int64_t last_store_ms = 0;
            int64_t        now_ms        = fmon_now_ms();

            if (now_ms - last_store_ms >= 10000) {
                fmon_history_store_sample(s);
                last_store_ms = now_ms;
            }
        }

        fmon_shm_wr_end();

        int64_t wait_ms = next_ms - fmon_now_ms();
        if (wait_ms > 0) {
            if (!fmon_sleep_ms((int)wait_ms) && !g_running)
                break;
        } else if (wait_ms < -(int64_t)FMON_SAMPLE_INTERVAL_MS) {
            next_ms = fmon_now_ms();   /* 落后太多就重新对齐 */
        }
    }

    return NULL;
}

int main(int argc, char **argv)
{
    char err[256];

    /* -V / --version：安装脚本与排错时用来确认版本/ABI/GPU 支持 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("fmon-daemon %s (共享内存 ABI %u, AMD GPU 传感器: %s)\n",
                   FMON_VERSION_STRING, FMON_ABI_VERSION,
#ifdef FMON_HAVE_AMDGPU
                   "启用"
#else
                   "未启用（未找到 libdrm_amdgpu）"
#endif
            );
            return EXIT_SUCCESS;
        }
    }

    /* -f / --force：强制接管共享内存（用于清理残留的旧实例） */
    bool force = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
            force = true;
    }

    ensure_path();

    g_use_syslog = !isatty(STDERR_FILENO);
    if (g_use_syslog)
        openlog("fmon-daemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    install_signal_handlers();

    g_shm = fmon_shm_create(&g_shm_fd, force, err, sizeof(err));
    if (g_shm == NULL) {
        dlog("初始化共享内存失败: %s", err);
        return EXIT_FAILURE;
    }

    fmon_shm_writer_init(g_shm);

    fill_static_info(g_shm);
    g_shm->daemon_start_us = fmon_now_us();

    /* 历史数组保持 0（不再伪造 5%/50%/99% 之类的假曲线）。
     * 界面在 history_count == 0 时显示“正在采集…”。 */

    if (fmon_ctl_start())
        dlog("控制通道已启动（GUI 的进程/服务操作将不再调用 pkexec）");
    else
        dlog("控制通道启动失败，GUI 将回退到 pkexec");

    dlog("共享内存 %s 已就绪 (abi=%u, %zu 字节, pid=%d)",
         FMON_SHM_NAME, FMON_ABI_VERSION, sizeof(FmonShmData), (int)getpid());
    dlog("主机: %s | 系统: %s | CPU: %s (%u 逻辑核)", g_shm->hostname,
         g_shm->sys_version, g_shm->cpu_model, (unsigned)g_shm->cpu_count);

    pthread_t t_cpu, t_io, t_net, t_storage, t_zfs, t_proc, t_pub, t_mem;
    bool ok_cpu, ok_io, ok_net, ok_storage, ok_zfs, ok_proc, ok_pub, ok_mem;

    ok_cpu     = start_thread("cpu", thread_cpu_mem, &t_cpu);
    ok_io      = start_thread("io", thread_io, &t_io);
    ok_net     = start_thread("net", thread_net, &t_net);
    ok_storage = start_thread("storage", thread_storage, &t_storage);
    ok_zfs     = start_thread("zfs", thread_zfs, &t_zfs);
    ok_mem     = start_thread("meminfo", thread_meminfo, &t_mem);

    if (!ok_mem)
        dlog("内存条采集线程启动失败（dmidecode 需要 root）");
    ok_proc    = start_thread("proc", thread_procs, &t_proc);
    ok_pub     = start_thread("publisher", thread_publisher, &t_pub);

    if (!ok_pub) {
        dlog("发布线程启动失败，守护进程无法继续");
        g_running = 0;
    }

    while (g_running)
        fmon_sleep_ms(500);

    dlog("收到退出信号，正在停止采集线程…");

    /* 采集线程最多 5 秒的等待（zpool 子进程有超时保护），这里逐个 join */
    if (ok_cpu)     pthread_join(t_cpu, NULL);
    if (ok_io)      pthread_join(t_io, NULL);
    if (ok_net)     pthread_join(t_net, NULL);
    if (ok_storage) pthread_join(t_storage, NULL);
    if (ok_zfs)     pthread_join(t_zfs, NULL);
    if (ok_proc)    pthread_join(t_proc, NULL);
    if (ok_pub)     pthread_join(t_pub, NULL);

    /* 通知界面：数据已停止更新。不 unlink 共享内存对象，
     * 这样界面可以区分“守护进程已退出”和“从未启动过”。 */
    fmon_ctl_stop();

    fmon_shm_wr_begin();
    g_shm->status = (int32_t)FMON_STATUS_STOPPED;
    g_shm->daemon_pid = 0;
    g_shm->last_update_us = 0;
    fmon_shm_wr_end();

    fmon_shm_detach(g_shm);
    g_shm = NULL;

    if (g_shm_fd >= 0) {
        close(g_shm_fd);   /* 释放 flock 单实例锁 */
        g_shm_fd = -1;
    }

    dlog("已安全退出。");
    if (g_use_syslog)
        closelog();

    return EXIT_SUCCESS;
}
