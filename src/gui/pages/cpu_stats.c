/*
 * cpu_stats.c - CPU 时间分布/频率/调度压力采样
 */

#include "cpu_stats.h"

#include <dirent.h>
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

/* ===========================================================================
 * 硬件虚拟化 / vmm / guest 状态
 *
 * 数据来源：
 *   CPUID 0/1/0x80000001/0x8000000A：CPU 厂商、VMX/SVM、NPT/NRIP 等特性；
 *   kern.vm_guest：当前是否运行在客户机；
 *   /dev/vmm：vmm 是否加载、正在运行的 VM 数；
 *   /boot/kernel/vmm.ko、bhyve：宿主能力是否就绪。
 * 不根据 CPU 型号字符串或发行版猜测。
 * ========================================================================= */
static gboolean sysctl_string(const char *name, char *buf, size_t len)
{
    size_t got = len;

    if (buf == NULL || len == 0)
        return FALSE;

    buf[0] = '\0';
    if (sysctlbyname(name, buf, &got, NULL, 0) != 0 || got == 0)
        return FALSE;

    buf[len - 1] = '\0';
    return buf[0] != '\0';
}

static void cpuid_leaf(guint32 leaf, guint32 sub, guint32 *a, guint32 *b,
                       guint32 *c, guint32 *d)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(sub));
#else
    *a = *b = *c = *d = 0;
#endif
}

static void virt_features_string(guint32 f, char *out, size_t out_len)
{
    out[0] = '\0';

    if (f & (1u << 0))
        g_strlcat(out, "NPT", out_len);
    if (f & (1u << 3))
        g_strlcat(out, "/NRIP", out_len);
    if (f & (1u << 5))
        g_strlcat(out, "/VClean", out_len);
    if (f & (1u << 6))
        g_strlcat(out, "/AFlush", out_len);
    if (f & (1u << 7))
        g_strlcat(out, "/DAssist", out_len);
    if (f & (1u << 1))
        g_strlcat(out, "/LBR", out_len);
}

static void collect_virt(CpuVirt *v)
{
    memset(v, 0, sizeof(*v));

    gboolean hypervisor_bit = FALSE;
    guint32  svm_nasids = 0;

#if defined(__x86_64__) || defined(__i386__)
    guint32 a, b, c, d;
    guint32 max_leaf, max_ext;

    cpuid_leaf(0, 0, &max_leaf, &b, &c, &d);

    char vendor[13];
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    cpuid_leaf(1, 0, &a, &b, &c, &d);
    v->vmx = (c & (1u << 5)) != 0;
    hypervisor_bit = (c & (1u << 31)) != 0;

    cpuid_leaf(0x80000000u, 0, &max_ext, &b, &c, &d);

    if (max_ext >= 0x80000001u) {
        cpuid_leaf(0x80000001u, 0, &a, &b, &c, &d);
        v->svm = (c & (1u << 2)) != 0;
    }

    if (v->svm && max_ext >= 0x8000000au) {
        cpuid_leaf(0x8000000au, 0, &a, &b, &c, &d);
        v->svm_features = d;
        svm_nasids = b;
        v->npt = (d & 1u) != 0;
    }

    if (strncmp(vendor, "AuthenticAMD", 12) == 0)
        g_strlcpy(v->hw_name, v->svm ? "AMD-V (SVM)" : "AMD (无 SVM)",
                  sizeof(v->hw_name));
    else if (strncmp(vendor, "GenuineIntel", 12) == 0)
        g_strlcpy(v->hw_name, v->vmx ? "Intel VT-x" : "Intel (无 VT-x)",
                  sizeof(v->hw_name));
    else
        g_strlcpy(v->hw_name, "未知 x86 厂商", sizeof(v->hw_name));
#endif

    v->hw_supported = v->svm || v->vmx;

    char guest[64];

    if (sysctl_string("kern.vm_guest", guest, sizeof(guest)) &&
        strcmp(guest, "none") != 0) {
        v->guest = TRUE;
        g_strlcpy(v->guest_name, guest, sizeof(v->guest_name));
    } else if (hypervisor_bit) {
        v->guest = TRUE;
        g_strlcpy(v->guest_name, "hypervisor", sizeof(v->guest_name));
    } else {
        g_strlcpy(v->guest_name, "none", sizeof(v->guest_name));
    }

    /* vmm 模块加载后 /dev/vmm 不一定已经出现；以 hw.vmm.maxcpu
     * 这个内核 sysctl 作为模块已加载的主判据，/dev/vmm 单独记录为
     * bhyve 设备目录是否可用。旧逻辑只看 /dev/vmm，会把已 kldload
     * 的 vmm 误报成“未加载”。 */
    int    vmm_maxcpu = 0;
    size_t vmmlen = sizeof(vmm_maxcpu);
    gboolean vmm_sysctl =
        sysctlbyname("hw.vmm.maxcpu", &vmm_maxcpu, &vmmlen, NULL, 0) == 0 &&
        vmm_maxcpu > 0;

    v->vmm_dev = access("/dev/vmm", F_OK) == 0;
    v->vmm_loaded = vmm_sysctl || v->vmm_dev;
    v->vmm_module = access("/boot/kernel/vmm.ko", F_OK) == 0;

    char *bhyve_path = g_find_program_in_path("bhyve");
    v->bhyve = (bhyve_path != NULL) || access("/usr/sbin/bhyve", X_OK) == 0;
    g_free(bhyve_path);

    if (v->vmm_dev) {
        DIR *dir = opendir("/dev/vmm");

        if (dir != NULL) {
            struct dirent *de;

            v->vm_list_valid = TRUE;

            while ((de = readdir(dir)) != NULL) {
                if (strcmp(de->d_name, ".") != 0 &&
                    strcmp(de->d_name, "..") != 0)
                    v->vm_count++;
            }
            closedir(dir);
        }

        int iommu = 0;
        size_t ilen = sizeof(iommu);

        if (sysctlbyname("hw.vmm.iommu.initialized", &iommu, &ilen, NULL,
                         0) == 0) {
            v->iommu_valid = TRUE;
            v->iommu = iommu != 0;
        }
    }

    char feats[160];
    virt_features_string(v->svm_features, feats, sizeof(feats));

    if (v->svm && svm_nasids > 0) {
        char asids[32];
        g_snprintf(asids, sizeof(asids), "%s%u ASIDs",
                   feats[0] ? " · " : "", svm_nasids);
        g_strlcat(feats, asids, sizeof(feats));
    }

    if (v->guest) {
        g_snprintf(v->state, sizeof(v->state), "%s 客户机",
                   v->guest_name);
        g_snprintf(v->detail, sizeof(v->detail),
                   "当前运行在 %s 客户机中；CPU 暴露 %s；vmm %s；"
                   "bhyve %s；VM 数 %u",
                   v->guest_name, v->hw_name,
                   v->vmm_loaded ? "已加载" : "未加载",
                   v->bhyve ? "可用" : "不可用", v->vm_count);
    } else if (v->vmm_loaded) {
        char vm_text[32];

        if (v->vm_list_valid)
            g_snprintf(vm_text, sizeof(vm_text), "%u 台", v->vm_count);
        else
            g_strlcpy(vm_text, "数未知", sizeof(vm_text));

        if (v->vmm_dev) {
            g_snprintf(v->state, sizeof(v->state), "%s · vmm 已加载",
                       v->svm ? "AMD-V" : "VT-x");
            g_snprintf(v->detail, sizeof(v->detail),
                       "%s%s%s · IOMMU %s · bhyve %s · 运行中 VM %s · 裸机",
                       v->hw_name, feats[0] ? " · " : "", feats,
                       v->iommu_valid ? (v->iommu ? "已启用" : "未启用")
                                      : "未知",
                       v->bhyve ? "可用" : "不可用", vm_text);
        } else {
            g_snprintf(v->state, sizeof(v->state), "%s · vmm 已加载",
                       v->svm ? "AMD-V" : "VT-x");
            g_snprintf(v->detail, sizeof(v->detail),
                       "%s%s%s · vmm 模块已加载 · /dev/vmm 不可用 · "
                       "IOMMU %s · bhyve %s · 裸机",
                       v->hw_name, feats[0] ? " · " : "", feats,
                       v->iommu_valid ? (v->iommu ? "已启用" : "未启用")
                                      : "未知",
                       v->bhyve ? "可用" : "不可用");
        }
    } else if (v->hw_supported) {
        g_snprintf(v->state, sizeof(v->state), "%s · vmm 未加载",
                   v->svm ? "AMD-V" : "VT-x");
        g_snprintf(v->detail, sizeof(v->detail),
                   "%s%s%s · vmm.ko %s · bhyve %s · 裸机 · "
                   "启用宿主虚拟化需 kldload vmm",
                   v->hw_name, feats[0] ? " · " : "", feats,
                   v->vmm_module ? "存在" : "不存在",
                   v->bhyve ? "可用" : "不可用");
    } else {
        g_strlcpy(v->state, "不支持硬件虚拟化", sizeof(v->state));
        g_snprintf(v->detail, sizeof(v->detail),
                   "CPUID 未暴露 VMX/SVM；vmm %s；bhyve %s；裸机",
                   v->vmm_loaded ? "已加载" : "未加载",
                   v->bhyve ? "可用" : "不可用");
    }
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

    s.base_mhz = (double)rd64("hw.clockrate");
    collect_virt(&s.virt);

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
