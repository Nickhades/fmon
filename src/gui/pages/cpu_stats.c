/*
 * cpu_stats.c - CPU 时间分布/频率/调度压力采样
 */

#include "cpu_stats.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/sysctl.h>

static CpuStats g_stats;
static gboolean g_valid = FALSE;

static guint64 g_prev_cp[FMON_MAX_CPUS][CPU_TIME_COUNT];
static guint64 g_prev_swtch, g_prev_intr;
static gint64  g_prev_us;

double cpu_stats_pct(guint64 part, guint64 total)
{
    if (total == 0)
        return 0.0;

    double p = 100.0 * (double)part / (double)total;

    return (p < 0.0) ? 0.0 : ((p > 100.0) ? 100.0 : p);
}

double cpu_stats_rate(guint64 cur, guint64 prev, double dt)
{
    if (dt <= 0.0 || cur < prev)
        return 0.0;

    return (double)(cur - prev) / dt;
}

static guint64 rd64(const char *name)
{
    guint64 v = 0;
    size_t  len = sizeof(v);

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return 0;

    return v;
}

static double read_temp(int cpu)
{
    char name[48];
    int  v = 0;
    size_t len = sizeof(v);

    g_snprintf(name, sizeof(name), "dev.cpu.%d.temperature", cpu);

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return -1.0;

    /* coretemp 返回开尔文×10；部分驱动直接返回摄氏度 */
    if (v > 200)
        return (double)v / 10.0 - 273.15;

    return (double)v;
}

static double read_max_freq(void)
{
    char  buf[256];
    size_t len = sizeof(buf);

    if (sysctlbyname("dev.cpu.0.freq_levels", buf, &len, NULL, 0) != 0)
        return 0.0;

    /* 形如 "3800/4560 2200/2420 ..."：每段取 '/' 前的频率，取最大值 */
    double best = 0.0;
    char  *save = NULL;

    for (char *tok = strtok_r(buf, " ", &save); tok != NULL;
         tok = strtok_r(NULL, " ", &save)) {
        long f = strtol(tok, NULL, 10);

        if ((double)f > best)
            best = (double)f;
    }

    return best;
}

void cpu_stats_collect(CpuStats *out)
{
    if (out == NULL)
        return;

    CpuStats s;
    long     cp[FMON_MAX_CPUS * CPU_TIME_COUNT];
    size_t   len = sizeof(cp);

    memset(&s, 0, sizeof(s));
    memset(cp, 0, sizeof(cp));

    if (sysctlbyname("kern.cp_times", cp, &len, NULL, 0) != 0)
        return;

    int ncpu = (int)(len / (sizeof(long) * CPU_TIME_COUNT));

    if (ncpu <= 0 || ncpu > FMON_MAX_CPUS)
        return;

    s.ncpu = ncpu;

    guint64 total = 0;
    guint64 sum[CPU_TIME_COUNT] = { 0 };

    for (int c = 0; c < ncpu; c++) {
        for (int t = 0; t < CPU_TIME_COUNT; t++) {
            guint64 cur = (guint64)cp[c * CPU_TIME_COUNT + t];
            guint64 prev = g_prev_cp[c][t];

            if (cur >= prev) {
                sum[t] += cur - prev;
                total  += cur - prev;
            }
            g_prev_cp[c][t] = cur;
        }
    }

    for (int t = 0; t < CPU_TIME_COUNT; t++)
        s.pct[t] = cpu_stats_pct(sum[t], total);

    char name[48];

    for (int c = 0; c < ncpu && c < FMON_MAX_CPUS; c++) {
        g_snprintf(name, sizeof(name), "dev.cpu.%d.freq", c);
        s.freq_mhz[c] = (double)rd64(name);
        s.temp_c[c]   = read_temp(c);
    }

    s.freq_max = read_max_freq();

    guint64 swtch = rd64("vm.stats.sys.v_swtch");
    guint64 intr  = rd64("vm.stats.sys.v_intr");
    gint64  now   = g_get_monotonic_time();
    double  dt    = (g_prev_us > 0) ? (double)(now - g_prev_us) / 1e6 : 0.0;

    s.cs_per_s = cpu_stats_rate(swtch, g_prev_swtch, dt);
    s.in_per_s = cpu_stats_rate(intr, g_prev_intr, dt);

    g_prev_swtch = swtch;
    g_prev_intr  = intr;
    g_prev_us    = now;

    /* 插槽数 = 逻辑处理器 / (每插槽核心 × 每核线程) */
    {
        int cores = 0, tpc = 0;
        size_t clen = sizeof(cores);

        s.sockets = 1;
        if (sysctlbyname("kern.smp.cores", &cores, &clen, NULL, 0) == 0 &&
            cores > 0) {
            clen = sizeof(tpc);
            if (sysctlbyname("kern.smp.threads_per_core", &tpc, &clen,
                             NULL, 0) != 0 || tpc <= 0)
                tpc = 1;

            int ncpu_total = (int)rd64("hw.ncpu");

            if (ncpu_total <= 0)
                ncpu_total = s.ncpu;

            s.sockets = ncpu_total / (cores * tpc);
            if (s.sockets < 1)
                s.sockets = 1;
        }
    }

    s.base_mhz     = (double)rd64("hw.clockrate");
    s.virt_enabled = (access("/dev/vmm", F_OK) == 0);

    /* vm.loadavg 是定点的 struct loadavg，不是 double[3] */
    struct loadavg la;
    size_t llen = sizeof(la);

    if (sysctlbyname("vm.loadavg", &la, &llen, NULL, 0) == 0 &&
        la.fscale > 0)
        s.load1 = (double)la.ldavg[0] / (double)la.fscale;

    *out = s;
}

void cpu_stats_publish(const CpuStats *in)
{
    if (in == NULL)
        return;

    g_valid = in->ncpu > 0;

    if (g_valid)
        g_stats = *in;
}

void cpu_stats_update(void)
{
    CpuStats s;

    memset(&s, 0, sizeof(s));
    cpu_stats_collect(&s);
    cpu_stats_publish(&s);
}

const CpuStats *cpu_stats_get(void)
{
    return g_valid ? &g_stats : NULL;
}

