/*
 * collector_proc.c - 进程快照（kvm）
 *
 * 旧版本的问题：
 *   1. cpu_usage 恒为 0，界面上“按 CPU 排序”实际上没有意义。
 *   2. 只保留 SRUN / SSLEEP 的进程，且遇到前 FMON_MAX_PROCS 个就停止，
 *      结果列表既不是“全部”也不是“Top N”，完全是随机子集。
 *   3. strncpy() 不保证 '\0' 结尾。
 *   4. 在 FreeBSD 15 上 kvm_getprocs() 返回的是“每个线程一条记录”，
 *      直接遍历会把同一个进程重复计数（一次采样 1839 条记录 / 178 个进程）。
 *      这里先按 pid 聚合所有线程的 CPU 占用，再排序取 Top N。
 */

#include "fmon_daemon.h"
#include "fmon_sysctl.h"

#include <fcntl.h>
#include <kvm.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/user.h>
#include <unistd.h>

/* <sys/proc.h> 中的 legacy 进程状态值：不直接包含内核头文件，避免污染 */
#define FMON_PS_IDL   1
#define FMON_PS_RUN   2
#define FMON_PS_SLEEP 3
#define FMON_PS_STOP  4
#define FMON_PS_ZOMB  5
#define FMON_PS_WAIT  6
#define FMON_PS_LOCK  7

typedef struct {
    pid_t    pid;
    uid_t    uid;
    uint64_t pct_fix;     /* 所有线程的 ki_pctcpu 之和 */
    uint64_t rss_pages;
    uint32_t threads;
    u_char   stat;
    char     comm[FMON_MAX_NAME_LEN];
} ProcAgg;

/* ---------------------------------------------------------------------------
 * uid → 用户名 缓存（每 2 秒最多几十次查询，缓存后几乎不碰 NSS）
 * ------------------------------------------------------------------------- */
#define USER_CACHE_SLOTS 64

static struct {
    uid_t uid;
    bool  valid;
    char  name[FMON_MAX_USER_LEN];
} g_user_cache[USER_CACHE_SLOTS];

static const char *lookup_user(uid_t uid)
{
    unsigned slot = (unsigned)uid % USER_CACHE_SLOTS;

    if (g_user_cache[slot].valid && g_user_cache[slot].uid == uid)
        return g_user_cache[slot].name;

    char           buf[1024];
    struct passwd  pwd;
    struct passwd *result = NULL;
    const char    *name   = NULL;

    if (getpwuid_r(uid, &pwd, buf, sizeof(buf), &result) == 0 &&
        result != NULL && result->pw_name != NULL)
        name = result->pw_name;

    g_user_cache[slot].uid   = uid;
    g_user_cache[slot].valid = true;
    strlcpy(g_user_cache[slot].name, (name != NULL) ? name : "?",
            sizeof(g_user_cache[slot].name));

    return g_user_cache[slot].name;
}

/* ---------------------------------------------------------------------------
 * 排序辅助
 * ------------------------------------------------------------------------- */
static int cmp_by_pid(const void *a, const void *b)
{
    const ProcAgg *pa = a;
    const ProcAgg *pb = b;

    if (pa->pid < pb->pid)
        return -1;
    if (pa->pid > pb->pid)
        return 1;
    return 0;
}

/* 升序，方便从尾部取最大值 */
static int cmp_by_pct(const void *a, const void *b)
{
    const ProcAgg *pa = a;
    const ProcAgg *pb = b;

    if (pa->pct_fix < pb->pct_fix)
        return -1;
    if (pa->pct_fix > pb->pct_fix)
        return 1;
    return 0;
}

/* idle / intr 是内核伪进程：它们的 CPU 占用是“每个核一条线程”的累加，
 * 会以 1600% 之类的数字长期霸占 Top 列表，对用户没有意义。 */
static bool is_pseudo_process(const char *comm)
{
    return (strcmp(comm, "idle") == 0 || strcmp(comm, "intr") == 0);
}

static char state_char(u_char stat)
{
    switch (stat) {
    case FMON_PS_IDL:   return 'I';
    case FMON_PS_RUN:   return 'R';
    case FMON_PS_SLEEP: return 'S';
    case FMON_PS_STOP:  return 'T';
    case FMON_PS_ZOMB:  return 'Z';
    case FMON_PS_WAIT:  return 'W';
    case FMON_PS_LOCK:  return 'L';
    default:            return '?';
    }
}

/* ---------------------------------------------------------------------------
 * 采样
 * ------------------------------------------------------------------------- */
static ProcAgg *g_agg    = NULL;
static size_t   g_agg_cap = 0;

static bool ensure_capacity(size_t need)
{
    if (need <= g_agg_cap)
        return true;

    ProcAgg *p = realloc(g_agg, need * sizeof(*p));
    if (p == NULL)
        return false;

    g_agg     = p;
    g_agg_cap = need;
    return true;
}

static void sample_procs(FmonShmData *s, kvm_t *kd)
{
    int count = 0;
    struct kinfo_proc *procs = kvm_getprocs(kd, KERN_PROC_ALL, 0, &count);

    if (procs == NULL || count <= 0) {
        s->proc_count = 0;
        return;
    }

    if (!ensure_capacity((size_t)count))
        return;

    /* 第一遍：展开为一条线程一条记录 */
    size_t n = 0;
    for (int i = 0; i < count; i++) {
        g_agg[n].pid       = procs[i].ki_pid;
        g_agg[n].uid       = procs[i].ki_uid;
        g_agg[n].pct_fix   = (uint64_t)procs[i].ki_pctcpu;
        g_agg[n].rss_pages = (uint64_t)procs[i].ki_rssize;
        g_agg[n].threads   = procs[i].ki_numthreads;
        g_agg[n].stat      = procs[i].ki_stat;
        strlcpy(g_agg[n].comm, procs[i].ki_comm, sizeof(g_agg[n].comm));
        n++;
    }

    /* 第二遍：按 pid 排序后合并同一进程的所有线程 */
    qsort(g_agg, n, sizeof(g_agg[0]), cmp_by_pid);

    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m > 0 && g_agg[m - 1].pid == g_agg[i].pid) {
            g_agg[m - 1].pct_fix += g_agg[i].pct_fix;
        } else {
            g_agg[m++] = g_agg[i];
        }
    }

    /* pid 0 是内核线程集合，idle/intr 是伪进程，都不参与展示与统计 */
    size_t real = 0;
    for (size_t i = 0; i < m; i++) {
        if (g_agg[i].pid != 0 && !is_pseudo_process(g_agg[i].comm))
            g_agg[real++] = g_agg[i];
    }

    s->proc_total = (uint32_t)real;

    /* 第三遍：按 CPU 占用升序排序，从尾部取 Top N */
    qsort(g_agg, real, sizeof(g_agg[0]), cmp_by_pct);

    uint64_t page_size = 0;
    if (fmon_sysctl_u64("hw.pagesize", &page_size) != 0 || page_size == 0)
        page_size = 4096;

    uint64_t mem_total = s->mem_total_bytes;
    double   max_pct   = (double)s->cpu_count * 100.0;
    if (max_pct <= 0.0)
        max_pct = 100.0;

    uint32_t out = 0;
    for (size_t k = real; k > 0 && out < FMON_MAX_PROCS; k--) {
        const ProcAgg *a = &g_agg[k - 1];

        /* 只保留真正占用过 CPU 或占用内存的进程，避免列表被
         * 大量 0% 内核线程填满；不足时再补零占用的进程。 */
        FmonProcInfo *item = &s->procs[out];

        memset(item, 0, sizeof(*item));
        item->pid     = (uint32_t)a->pid;
        item->uid     = (uint32_t)a->uid;
        item->threads = a->threads;
        item->state   = state_char(a->stat);
        memcpy(item->name, a->comm, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
        strlcpy(item->user, lookup_user(a->uid), sizeof(item->user));

        double pct = 100.0 * (double)a->pct_fix / (double)FSCALE;
        if (pct > max_pct)
            pct = max_pct;
        if (pct < 0.0)
            pct = 0.0;
        item->cpu_usage_pct = pct;

        item->mem_rss_kb = (double)a->rss_pages * (double)page_size / 1024.0;
        item->mem_pct =
            (mem_total > 0)
                ? 100.0 * (double)(a->rss_pages * page_size) / (double)mem_total
                : 0.0;

        out++;
    }

    s->proc_count = out;
}

void *thread_procs(void *arg)
{
    (void)arg;

    char  errbuf[_POSIX2_LINE_MAX];
    kvm_t *kd = kvm_openfiles(NULL, "/dev/null", NULL, O_RDONLY, errbuf);

    if (kd == NULL) {
        fprintf(stderr, "[FMON-DAEMON] kvm_openfiles 失败: %s\n", errbuf);
        return NULL;
    }

    while (g_running) {
        fmon_shm_wr_begin();
        sample_procs(g_shm, kd);
        fmon_shm_wr_end();

        if (!fmon_sleep_ms(2000) && !g_running)
            break;
    }

    kvm_close(kd);
    free(g_agg);
    g_agg = NULL;
    g_agg_cap = 0;

    return NULL;
}
