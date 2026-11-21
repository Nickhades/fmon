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

/* CPU 硬件虚拟化能力 + vmm/guest 状态。全部来自 CPUID / sysctl /
 * 内核设备节点，不根据发行版或机器名猜测。 */
typedef struct {
    gboolean hw_supported;      /* CPUID 暴露 VMX 或 SVM */
    gboolean vmx;               /* Intel VT-x */
    gboolean svm;               /* AMD-V */
    gboolean guest;             /* 当前运行在客户机中 */
    gboolean vmm_loaded;        /* vmm 模块已加载（sysctl 或 /dev/vmm） */
    gboolean vmm_dev;           /* /dev/vmm 设备目录可用 */
    gboolean vmm_module;        /* /boot/kernel/vmm.ko 存在 */
    gboolean bhyve;             /* bhyve 用户态可用 */
    gboolean npt;               /* AMD NPT（Intel EPT 需 vmm 加载后才能确认） */
    gboolean iommu;             /* vmm 报告 IOMMU 已初始化 */
    gboolean iommu_valid;       /* hw.vmm.iommu.initialized 可读 */
    gboolean vm_list_valid;     /* /dev/vmm 可枚举 */
    guint32  svm_features;      /* CPUID 0x8000000A EDX */
    guint32  vm_count;          /* /dev/vmm 下正在运行的 VM 数 */
    char     guest_name[32];    /* kern.vm_guest */
    char     hw_name[32];       /* AMD-V (SVM) / Intel VT-x */
    char     features[128];     /* NPT/NRIP/VClean/... */
    char     state[64];         /* 紧凑状态 */
    char     detail[256];       /* 完整真实数据 */
} CpuVirt;

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
    CpuVirt virt;                     /* 硬件虚拟化/vmm/guest 完整状态 */
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
