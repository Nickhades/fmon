/*
 * cpu_stats.h - CPU 时间分布、频率与调度压力（GUI 侧采样）
 *
 * daemon 只发布总使用率和每核占用；时间分布（user/nice/sys/intr/idle）、
 * 实时频率、上下文切换/中断速率由界面直接读 sysctl：
 *   kern.cp_times、dev.cpu.N.freq、dev.cpu.0.freq_levels、
 *   dev.cpu.N.temperature（可选）、vm.stats.sys.v_swtch / v_intr、vm.loadavg
 * 这些都不需要 root，1 Hz 采样开销很小，因此不动共享内存 ABI。
 */

#ifndef CPU_STATS_H
#define CPU_STATS_H

#include <glib.h>

#include "fmon_shm.h"

enum {
    CPU_TIME_USER = 0,
    CPU_TIME_NICE,
    CPU_TIME_SYS,
    CPU_TIME_INTR,
    CPU_TIME_IDLE,
    CPU_TIME_COUNT
};

typedef struct {
    int    ncpu;
    double pct[CPU_TIME_COUNT];       /* 总体时间占比（0–100） */

    double freq_mhz[FMON_MAX_CPUS];   /* 每核当前频率，0 = 不可用 */
    double freq_max;                  /* 频率档位里的最高档 */
    double temp_c[FMON_MAX_CPUS];     /* 每核温度，<0 = 不可用 */

    double cs_per_s;                  /* 上下文切换/秒 */
    double in_per_s;                  /* 中断/秒 */
    double load1;                     /* 1 分钟负载（r 队列的近似） */

    int    sockets;                   /* 物理插槽数 */
    double base_mhz;                  /* 标称频率（hw.clockrate） */
    gboolean virt_enabled;            /* /dev/vmm 是否存在 */
} CpuStats;

void cpu_stats_update(void);

/* 采集线程用：collect 只读 sysctl，publish 在主线程发布。 */
void cpu_stats_collect(CpuStats *out);
void cpu_stats_publish(const CpuStats *in);
const CpuStats *cpu_stats_get(void);

/* 纯函数：时间占比与计数器速率 */
double cpu_stats_pct(guint64 part, guint64 total);
double cpu_stats_rate(guint64 cur, guint64 prev, double dt);

#endif /* CPU_STATS_H */
