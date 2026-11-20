/*
 * collector_cpu.c - CPU / 每核负载 / 温度 / 内存 / 负载均值 / 电池
 *
 * 相对旧版本的主要修复：
 *   1. sysctlbyname() 的 len 参数在两次调用之间没有重置，第二个调用可能
 *      用错误的长度写入；现在每次都重新初始化。
 *   2. 电池“充电中”的判断原来用 state == 3，而 FreeBSD 的充电位是 0x02，
 *      导致充电时仍显示放电功率；现在按位判断，并增加了交流电状态。
 *   3. 增加了 kern.cp_times 每核采样（新功能），在界面里以柱状图显示。
 *   4. CPU 占用用“本次 / 上次”的差值计算，并对计数器回绕做了保护。
 *   5. 删除“温度默认 45℃”这类伪造值：没有传感器时上报 NaN，由界面显示 N/A。
 */

#include "fmon_daemon.h"
#include "fmon_sysctl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define FMON_CPUSTATES CPUSTATES   /* 5：user / nice / sys / intr / idle */

/* ===========================================================================
 * 温度
 * ========================================================================= */

/* 单个温度传感器，返回摄氏度（内核单位是 0.1K） */
static bool read_temp_sensor(const char *name, double *out_c)
{
    int    raw = 0;
    size_t len = sizeof(raw);

    if (sysctlbyname(name, &raw, &len, NULL, 0) != 0)
        return false;
    if (len != sizeof(raw))
        return false;

    /* NaN / Inf 也会让下面的比较为假，因此无需 isfinite()，也就不依赖 libm */
    double c = (double)raw / 10.0 - 273.15;
    if (!(c > 0.0 && c < 130.0))
        return false;

    *out_c = c;
    return true;
}

/* 取所有核心温度的最大值；没有核心传感器时退回 ACPI 热区。
 * 返回 -1 表示本次没有可用的读数。 */
static double read_cpu_temp(unsigned cpu_count)
{
    char   name[64];
    double best = -1.0;
    double c    = 0.0;

    unsigned limit = (cpu_count == 0) ? 1u : cpu_count;
    if (limit > FMON_MAX_CPUS)
        limit = FMON_MAX_CPUS;

    for (unsigned i = 0; i < limit; i++) {
        snprintf(name, sizeof(name), "dev.cpu.%u.temperature", i);
        if (read_temp_sensor(name, &c) && c > best)
            best = c;
    }

    if (best < 0.0 && read_temp_sensor("hw.acpi.thermal.tz0.temperature", &c))
        best = c;

    return best;
}

/* 温度不做 EMA/滑动平均：传感器读数是监控对象本身，采集层和渲染层
 * 都不应该替用户“降噪”。读不到传感器时写 NaN，界面显示“不可用”；
 * 绘图端遇到 NaN 会断开折线，不会把缺失值画成 0°C。 */

/* ===========================================================================
 * CPU 总占用
 * ========================================================================= */
static void sample_cpu_usage(FmonShmData *s)
{
    static uint64_t prev[FMON_CPUSTATES];
    static bool     primed = false;

    long   cur[FMON_CPUSTATES];
    size_t len = sizeof(cur);

    if (sysctlbyname("kern.cp_time", cur, &len, NULL, 0) != 0)
        return;
    if (len != sizeof(cur))
        return;

    uint64_t now[FMON_CPUSTATES];
    for (int i = 0; i < FMON_CPUSTATES; i++)
        now[i] = (uint64_t)cur[i];

    if (primed) {
        uint64_t dtotal = 0;
        uint64_t didle  = 0;

        for (int i = 0; i < FMON_CPUSTATES; i++) {
            if (now[i] >= prev[i])
                dtotal += now[i] - prev[i];
        }
        if (now[CP_IDLE] >= prev[CP_IDLE])
            didle = now[CP_IDLE] - prev[CP_IDLE];

        if (dtotal > 0) {
            if (didle > dtotal)
                didle = dtotal;
            double pct = 100.0 * (double)(dtotal - didle) / (double)dtotal;
            if (pct >= 0.0 && pct <= 100.0)
                s->cpu_usage_pct = pct;
        }
    }

    memcpy(prev, now, sizeof(prev));
    primed = true;
}

/* ===========================================================================
 * 每核占用（新功能）
 * ========================================================================= */
static void sample_cpu_cores(FmonShmData *s)
{
    static uint64_t prev[FMON_MAX_CPUS][FMON_CPUSTATES];
    static uint16_t prev_cores = 0;
    static bool     primed     = false;

    size_t len = 0;
    if (sysctlbyname("kern.cp_times", NULL, &len, NULL, 0) != 0)
        return;
    if (len < sizeof(long) * FMON_CPUSTATES)
        return;

    unsigned cores = (unsigned)(len / (sizeof(long) * FMON_CPUSTATES));
    if (cores > FMON_MAX_CPUS)
        cores = FMON_MAX_CPUS;

    long   buf[FMON_MAX_CPUS * FMON_CPUSTATES];
    size_t want = sizeof(long) * FMON_CPUSTATES * cores;

    memset(buf, 0, sizeof(buf));
    if (sysctlbyname("kern.cp_times", buf, &want, NULL, 0) != 0)
        return;
    if (want < sizeof(long) * FMON_CPUSTATES * cores)
        return;

    for (unsigned c = 0; c < cores; c++) {
        uint64_t cur[FMON_CPUSTATES];

        for (int st = 0; st < FMON_CPUSTATES; st++)
            cur[st] = (uint64_t)buf[c * FMON_CPUSTATES + st];

        double pct = 0.0;

        if (primed && c < prev_cores) {
            uint64_t dtotal = 0;
            uint64_t didle  = 0;

            for (int st = 0; st < FMON_CPUSTATES; st++) {
                if (cur[st] >= prev[c][st])
                    dtotal += cur[st] - prev[c][st];
            }
            if (cur[CP_IDLE] >= prev[c][CP_IDLE])
                didle = cur[CP_IDLE] - prev[c][CP_IDLE];

            if (dtotal > 0) {
                if (didle > dtotal)
                    didle = dtotal;
                pct = 100.0 * (double)(dtotal - didle) / (double)dtotal;
            }
        }

        memcpy(prev[c], cur, sizeof(cur));
        s->cpu_core_pct[c] = pct;
    }

    s->cpu_core_count = (uint16_t)cores;
    prev_cores        = (uint16_t)cores;
    primed            = true;
}

/* ===========================================================================
 * 内存
 * ========================================================================= */
static void sample_memory(FmonShmData *s)
{
    static uint64_t page_size = 0;

    if (page_size == 0) {
        uint64_t ps = 0;
        if (fmon_sysctl_u64("hw.pagesize", &ps) == 0 && ps > 0)
            page_size = ps;
        else
            page_size = 4096;
    }

    uint64_t pages = 0, free_pages = 0, inactive_pages = 0, cache_pages = 0;

    if (fmon_sysctl_u64("vm.stats.vm.v_page_count", &pages) != 0 || pages == 0)
        return;
    (void)fmon_sysctl_u64("vm.stats.vm.v_free_count", &free_pages);
    (void)fmon_sysctl_u64("vm.stats.vm.v_inactive_count", &inactive_pages);
    (void)fmon_sysctl_u64("vm.stats.vm.v_cache_count", &cache_pages);

    uint64_t total = pages * page_size;
    uint64_t avail = (free_pages + inactive_pages + cache_pages) * page_size;

    if (avail > total)
        avail = total;

    s->mem_total_bytes = total;
    s->mem_free_bytes  = free_pages * page_size;
    s->mem_avail_bytes = avail;

    double pct = 100.0 * (double)(total - avail) / (double)total;
    if (pct < 0.0)
        pct = 0.0;
    if (pct > 100.0)
        pct = 100.0;
    s->mem_used_pct = pct;
}

/* ===========================================================================
 * 负载均值
 * ========================================================================= */
static void sample_loadavg(FmonShmData *s)
{
    struct loadavg la;
    size_t len = sizeof(la);

    if (sysctlbyname("vm.loadavg", &la, &len, NULL, 0) != 0)
        return;
    if (len != sizeof(la) || la.fscale <= 0)
        return;

    for (int i = 0; i < 3; i++)
        s->load_avg[i] = (double)la.ldavg[i] / (double)la.fscale;
}

/* ===========================================================================
 * 电池 / 电源
 * ========================================================================= */
static void sample_battery(FmonShmData *s)
{
    int life  = 0;
    int state = 0;
    int rate  = 0;

    if (fmon_sysctl_int("hw.acpi.battery.life", &life) != 0 ||
        life < 0 || life > 100) {
        s->battery_present     = 0;
        s->battery_pct         = -1.0;
        s->battery_discharge_w = 0.0;
        s->battery_flags       = 0;
        s->acline              = 0;
        s->probe_valid &= ~FMON_PROBE_BATTERY;
        return;
    }

    s->battery_present = 1;
    s->battery_pct     = (double)life;
    s->probe_valid |= FMON_PROBE_BATTERY;

    if (fmon_sysctl_int("hw.acpi.battery.state", &state) != 0)
        state = 0;

    uint32_t flags = 0;
    if ((state & FMON_BATT_CHARGING) != 0)
        flags |= FMON_BATT_CHARGING;
    if ((state & FMON_BATT_DISCHARGING) != 0)
        flags |= FMON_BATT_DISCHARGING;
    if ((state & FMON_BATT_CRITICAL) != 0)
        flags |= FMON_BATT_CRITICAL;

    /* 有些机型满电时不置充电位，用“100% 且未放电”补充判断 */
    if ((flags & FMON_BATT_DISCHARGING) == 0 && life >= 100)
        flags |= FMON_BATT_CHARGING;

    s->battery_flags = flags;

    int acline = 0;
    if (fmon_sysctl_int("hw.acpi.acline", &acline) == 0)
        s->acline = (acline > 0) ? 1u : 0u;
    else
        s->acline = 0;   /* 未知，不按放电标志猜测 */

    /* 只有放电时才有意义。hw.acpi.battery.rate 单位是 mW。 */
    if ((flags & FMON_BATT_DISCHARGING) != 0 &&
        fmon_sysctl_int("hw.acpi.battery.rate", &rate) == 0 && rate > 0) {
        s->battery_discharge_w = (double)rate / 1000.0;
    } else {
        s->battery_discharge_w = 0.0;
    }
}

/* ===========================================================================
 * 线程主循环
 * ========================================================================= */
void *thread_cpu_mem(void *arg)
{
    (void)arg;

    while (g_running) {
        double temp = read_cpu_temp(g_shm->cpu_count);

        /* 写事务尽量短：sysctl 读取都放在锁内完成，但每次只有几十微秒 */
        fmon_shm_wr_begin();
        sample_cpu_usage(g_shm);
        sample_cpu_cores(g_shm);
        sample_memory(g_shm);
        sample_loadavg(g_shm);
        sample_battery(g_shm);
        g_shm->cpu_temp_c =
            (isfinite(temp) && temp >= 0.0) ? temp : NAN;
        fmon_shm_wr_end();

        if (!fmon_sleep_ms(FMON_SAMPLE_INTERVAL_MS) && !g_running)
            break;
    }

    return NULL;
}
