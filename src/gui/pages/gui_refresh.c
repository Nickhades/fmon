/*
 * gui_refresh.c - 每秒页面刷新与主题重绘
 *
 * 只读 gui_state_snapshot() 的快照，更新各页面数值并请求重绘。
 */

#include "gui_refresh.h"

#include "gui_page_state.h"
#include "gui_pages.h"
#include "gui_coreview.h"
#include "gui_page_net.h"
#include "gui_page_disk.h"
#include "gui_page_mem.h"
#include "gui_page_l2arc.h"
#include "gui_page_batt.h"
#include "gui_page_cpu.h"
#include "cpu_stats.h"
#include "net_stats.h"
#include "disk_perf.h"
#include "gui_page_zfs.h"
#include "zfs_stats.h"
#include "disk_queue.h"
#include "collector_thread.h"
#include "mem_summary.h"
#include "gui_page_gpu.h"
#include "gui_state.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <stdlib.h>
#include <string.h>
#include <malloc_np.h>

static void queue_draw_maybe(GtkWidget *w);
static guint g_refresh_tick;
#include <sys/sysctl.h>

static void cset(ComboPage *p, int i, const char *text)
{
    if (i >= 0 && i < 12 && p->vals[i] != NULL)
        gtk_label_set_text(GTK_LABEL(p->vals[i]), text);
}

static void upd_cpu_combo(void)
{
    ComboPage *p = &g_cpu_page;
    char       b[96];

    /* 型号在标题行右侧（gui_cpu_hw_update），副标题保持固定说明 */

    int    sock  = 1;
    int64_t base = 0, cur = 0;

    {
        int cores = 0, tpc = 0;
        size_t len = sizeof(cores);

        if (sysctlbyname("kern.smp.cores", &cores, &len, NULL, 0) == 0 &&
            cores > 0) {
            len = sizeof(tpc);
            if (sysctlbyname("kern.smp.threads_per_core", &tpc, &len, NULL,
                             0) != 0 || tpc <= 0)
                tpc = 1;

            int total = (int)gui_state_snapshot()->cpu_count;
            sock = total / (cores * tpc);
            if (sock < 1)
                sock = 1;
        }
    }

    {
        int v = 0;
        size_t len = sizeof(v);

        if (sysctlbyname("hw.clockrate", &v, &len, NULL, 0) == 0 && v > 0)
            base = v;

        len = sizeof(v);
        if (sysctlbyname("dev.cpu.0.freq", &v, &len, NULL, 0) == 0 && v > 0)
            cur = v;
    }

    g_snprintf(b, sizeof(b), "%d", sock);
    cset(p, 0, b);

    g_snprintf(b, sizeof(b), "%u", gui_state_snapshot()->cpu_count);
    cset(p, 1, b);

    if (base > 0)
        g_snprintf(b, sizeof(b), "%.2f GHz", (double)base / 1000.0);
    else
        g_snprintf(b, sizeof(b), "—");
    cset(p, 2, b);

    if (cur > 0)
        g_snprintf(b, sizeof(b), "%.2f GHz", (double)cur / 1000.0);
    else
        g_snprintf(b, sizeof(b), "—");
    cset(p, 3, b);

    cset(p, 4, (access("/dev/vmm", F_OK) == 0) ? "已启用（vmm）" : "未启用");

    g_snprintf(b, sizeof(b), "%.2f / %.2f / %.2f", gui_state_snapshot()->load_avg[0],
               gui_state_snapshot()->load_avg[1], gui_state_snapshot()->load_avg[2]);
    cset(p, 5, b);
}

static void upd_zsum(void)
{
    ComboPage *p = &g_zsum_page;
    int        i = 0;
    char       b[96];

    const FmonShmData *snap = gui_state_snapshot();
    const FmonPoolInfo *z =
        ((snap->probe_valid & FMON_PROBE_POOLS) != 0 && snap->pool_count > 0 &&
         snap->pools[0].available)
            ? &snap->pools[0]
            : NULL;

    cset(p, i++, z ? z->name : "无可用池");
    cset(p, i++, z ? z->health : "—");

    g_snprintf(b, sizeof(b), "%.2f / %.2f GB",
               z ? (double)z->alloc_bytes / (1024.0 * 1024.0 * 1024.0) : 0.0,
               z ? (double)z->total_bytes / (1024.0 * 1024.0 * 1024.0) : 0.0);
    cset(p, i++, b);

    g_snprintf(b, sizeof(b), "%.1f %%", z ? z->util_pct : 0.0);
    cset(p, i++, b);

    double arc = (double)gui_state_snapshot()->zfs_arc_size / (1024.0 * 1024.0 * 1024.0);
    double max = (double)gui_state_snapshot()->zfs_arc_c_max / (1024.0 * 1024.0 * 1024.0);

    g_snprintf(b, sizeof(b), "%.2f GB", arc);
    cset(p, i++, b);
    g_snprintf(b, sizeof(b), "%.2f GB", max);
    cset(p, i++, b);
    double mem_total = (double)gui_state_snapshot()->mem_total_bytes;

    g_snprintf(b, sizeof(b), "%.1f %%",
               (mem_total > 0.0)
                   ? 100.0 * (double)gui_state_snapshot()->zfs_arc_size / mem_total
                   : 0.0);
    cset(p, i++, b);

    g_snprintf(b, sizeof(b), "%.1f %%", gui_state_snapshot()->zfs_arc_pct);
    cset(p, i++, b);

    g_snprintf(b, sizeof(b), "%llu / %llu",
               (unsigned long long)gui_state_snapshot()->zfs_l2_hits,
               (unsigned long long)gui_state_snapshot()->zfs_l2_misses);
    cset(p, i++, b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)(gui_state_snapshot()->zfs_mru_ghost_hits +
                                    gui_state_snapshot()->zfs_mfu_ghost_hits));
    cset(p, i++, b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)gui_state_snapshot()->zfs_arc_prune);
    cset(p, i++, b);
}

static void upd_net_combo(void)
{
    if (g_net_page.area == NULL)
        return;

    g_zpool_cd.count = gui_state_snapshot()->history_count;
    g_zpool_cd.index = gui_state_snapshot()->history_index;
    g_zpool_cd.time_us = gui_state_snapshot()->history_time_us;

    g_net_rx_cd.time_us = gui_state_snapshot()->history_time_us;
    g_net_tx_cd.time_us = gui_state_snapshot()->history_time_us;
    g_net_rx_cd.count = gui_state_snapshot()->history_count;
    g_net_rx_cd.index = gui_state_snapshot()->history_index;
    g_net_tx_cd.count = gui_state_snapshot()->history_count;
    g_net_tx_cd.index = gui_state_snapshot()->history_index;

    queue_draw_maybe(g_net_page.area);

    if (g_net_summary != NULL) {
        guint64 rx_total = 0, tx_total = 0;

        for (uint32_t i = 0; i < gui_state_snapshot()->iface_count && i < FMON_MAX_IFACES; i++) {
            rx_total += gui_state_snapshot()->ifaces[i].rx_bytes;
            tx_total += gui_state_snapshot()->ifaces[i].tx_bytes;
        }

        char *r = fmon_gui_format_bytes(rx_total);
        char *t = fmon_gui_format_bytes(tx_total);
        char  b[192];

        g_snprintf(b, sizeof(b), "接口 %u 个 · 累计 收 %s / 发 %s",
                   gui_state_snapshot()->iface_count, r, t);
        gtk_label_set_text(GTK_LABEL(g_net_summary), b);
        g_free(r);
        g_free(t);
    }
}

static void upd_disk_combo(void)
{
    if (g_disk_page.area == NULL)
        return;

    g_disk_rd_cd.time_us = gui_state_snapshot()->history_time_us;
    g_disk_wr_cd.time_us = gui_state_snapshot()->history_time_us;
    g_disk_rd_cd.count = gui_state_snapshot()->history_count;
    g_disk_rd_cd.index = gui_state_snapshot()->history_index;
    g_disk_wr_cd.count = gui_state_snapshot()->history_count;
    g_disk_wr_cd.index = gui_state_snapshot()->history_index;

    queue_draw_maybe(g_disk_page.area);

    if (g_disk_summary != NULL) {
        char b[192];

        /* 繁忙度由右上角 devstat 实时行显示，这里只保留累计次数，避免重复 */
        g_snprintf(b, sizeof(b), "累计读次 %llu / 写次 %llu",
                   (unsigned long long)gui_state_snapshot()->disk_read_ops,
                   (unsigned long long)gui_state_snapshot()->disk_write_ops);
        gtk_label_set_text(GTK_LABEL(g_disk_summary), b);
    }
}

static void upd_mem_combo(void)
{
    gui_mem_queues_update();
    ComboPage *p = &g_mem_page;
    int        i = 0;
    char       b[96];

    double mtotal = (double)gui_state_snapshot()->mem_total_bytes / (1024.0 * 1024.0 * 1024.0);
    uint64_t avail_b = gui_state_snapshot()->mem_avail_bytes;

    if (avail_b == 0)
        avail_b = gui_state_snapshot()->mem_total_bytes - gui_state_snapshot()->mem_free_bytes;

    double mavail = (double)avail_b / (1024.0 * 1024.0 * 1024.0);
    double mused  = mtotal - mavail;

    g_snprintf(b, sizeof(b), "%.2f GB", mtotal);
    cset(p, i++, b);
    g_snprintf(b, sizeof(b), "%.2f GB", mused);
    cset(p, i++, b);
    g_snprintf(b, sizeof(b), "%.2f GB", mavail);
    cset(p, i++, b);

    /* 说明行：ZFS ARC 属于内核缓存，内存紧张时会被回收，
     * 所以把“已用里有多少可回收”算出来，避免被 88% 吓到。
     * 文案生成在 mem_summary.c（纯函数，tests/test_mem_summary.c 覆盖）。 */
    if (g_mem_note != NULL) {
        gchar *note = gui_mem_summary_text(
            gui_state_snapshot()->mem_total_bytes, avail_b,
            gui_state_snapshot()->zfs_arc_size);

        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(g_mem_note)), note) != 0)
            gtk_label_set_text(GTK_LABEL(g_mem_note), note);

        g_free(note);
    }

    const FmonShmData *snap = gui_state_snapshot();

    if ((snap->probe_valid & FMON_PROBE_SWAP) == 0) {
        cset(p, i++, "不可用");
        cset(p, i++, "不可用");
    } else {
        double stotal = (double)snap->swap_total_kb / (1024.0 * 1024.0);
        double sused  = (double)snap->swap_used_kb / (1024.0 * 1024.0);

        g_snprintf(b, sizeof(b), "%.2f GB", stotal);
        cset(p, i++, b);
        g_snprintf(b, sizeof(b), "%.2f GB", sused);
        cset(p, i++, b);
    }
}

static void upd_zbw(void)
{
    /* 累计/操作/队列已移到曲线下方的“带宽明细”，带内只留速率 */
    g_zbw_chart.series[0].extra[0] = '\0';
    g_zbw_chart.series[1].extra[0] = '\0';
}

/* 主循环慢调用检测：>5ms 记一次，最多 60 秒一条，避免刷屏 */
static gint cmp_u64(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;

    guint64 x = *(const guint64 *)a;
    guint64 y = *(const guint64 *)b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void refresh_stats_sample(gint64 us)
{
    static guint64 samples[4096];
    static guint   n;
    static gint64  last_report;

    if (getenv("FMON_REFRESH_STATS") == NULL)
        return;

    samples[n % G_N_ELEMENTS(samples)] = (guint64)us;
    n++;

    gint64 now = g_get_monotonic_time();

    if (last_report != 0 && now - last_report < 60 * G_TIME_SPAN_SECOND)
        return;

    last_report = now;

    guint   cnt = MIN(n, (guint)G_N_ELEMENTS(samples));
    guint64 *sorted = g_new(guint64, cnt);
    guint64 sum = 0, mx = 0;

    for (guint i = 0; i < cnt; i++) {
        sorted[i] = samples[i];
        sum += samples[i];

        if (samples[i] > mx)
            mx = samples[i];
    }

    g_sort_array(sorted, cnt, sizeof(guint64),
                 (GCompareDataFunc)cmp_u64, NULL);

    g_printerr("refresh-stats n=%u avg=%.2f p50=%.2f p95=%.2f p99=%.2f "
               "max=%.2f ms\n",
               n, (double)sum / (double)cnt / 1000.0,
               (double)sorted[cnt / 2] / 1000.0,
               (double)sorted[(guint)((double)cnt * 0.95)] / 1000.0,
               (double)sorted[(guint)((double)cnt * 0.99)] / 1000.0,
               (double)mx / 1000.0);

    g_free(sorted);
}

static void queue_draw_maybe(GtkWidget *w)
{
    if (w == NULL || getenv("FMON_NO_REDRAW") != NULL)
        return;

    /* 不可见的页面不排重绘：GtkStack 隐藏页不需要每帧重画 */
    if (!gtk_widget_get_mapped(w))
        return;

    /* 调试/降频：FMON_DRAW_EVERY=N 表示每 N 个刷新周期才重绘一次 */
    const char *every = getenv("FMON_DRAW_EVERY");

    if (every != NULL) {
        int n = atoi(every);

        if (n > 1 && (g_refresh_tick % (guint)n) != 0)
            return;
    }

    gtk_widget_queue_draw(w);
}

static void warn_if_slow(const char *what, gint64 t0)
{
    static gint64 last_warn;
    gint64 us = g_get_monotonic_time() - t0;

    if (us <= 5000)
        return;

    gint64 now = g_get_monotonic_time();

    if (last_warn != 0 && now - last_warn < 60 * G_TIME_SPAN_SECOND)
        return;

    last_warn = now;
    g_warning("main-loop slow: %s took %.2f ms", what, (double)us / 1000.0);
}

static void heap_stats_sample(void)
{
    static gint64 last;

    if (getenv("FMON_HEAP_STATS") == NULL)
        return;

    gint64 now = g_get_monotonic_time();

    if (last != 0 && now - last < 60 * G_TIME_SPAN_SECOND)
        return;

    last = now;

    size_t sz = sizeof(size_t);
    size_t alloc = 0, active = 0, resident = 0;

    mallctl("stats.allocated", &alloc, &sz, NULL, 0);
    mallctl("stats.active", &active, &sz, NULL, 0);
    mallctl("stats.resident", &resident, &sz, NULL, 0);

    g_printerr("heap-stats allocated=%zu active=%zu resident=%zu\n", alloc,
               active, resident);
}

#define CALL_TIME(fn)                                                       \
    do {                                                                    \
        gint64 _ct = g_get_monotonic_time();                                \
        fn;                                                                 \
        gint64 _cd = g_get_monotonic_time() - _ct;                          \
        if (_cd > 500 && getenv("FMON_REFRESH_STAGE") != NULL)              \
            g_printerr("  call %s %.2f ms\n", #fn,                          \
                       (double)_cd / 1000.0);                               \
    } while (0)

void gui_pages_refresh(const FmonShmData *snap)
{
    g_refresh_tick++;
    heap_stats_sample();

    if (getenv("FMON_NO_PAGES") != NULL)
        return;

    gint64 t0 = g_get_monotonic_time();

    /* 常驻采集线程已接管 sysctl 类采集；旧路径保留为回退开关 */
    if (!gui_collector_active()) {
        /* ZFS 细节走 kstat（普通用户可读），每秒采样一次，供各 ZFS 页使用 */
        gint64 ts = g_get_monotonic_time();
        zfs_stats_update();
        warn_if_slow("zfs_stats", ts);

        /* CPU 时间分布/频率/调度压力 */
        ts = g_get_monotonic_time();
        cpu_stats_update();
        warn_if_slow("cpu_stats", ts);

        /* 网络链路错误/丢包与 TCP 统计 */
        ts = g_get_monotonic_time();
        net_stats_update();
        warn_if_slow("net_stats", ts);

        /* 磁盘 IOPS / 延迟 / 繁忙（devstat） */
        ts = g_get_monotonic_time();
        disk_perf_update();
        warn_if_slow("disk_perf", ts);
    }

    gui_disk_temp_sample();

    gui_metric_curves_sync(snap);

    gint64 tm[3];

    tm[0] = g_get_monotonic_time();

    for (int i = 0; i < METRIC_PAGE_COUNT; i++) {
        MetricPage *mp = &g_metrics[i];

        if (mp->update != NULL)
            mp->update(mp);

        if (mp->area != NULL)
            queue_draw_maybe(mp->area);
    }

    tm[1] = g_get_monotonic_time();

    CALL_TIME(upd_cpu_combo());
    CALL_TIME(gui_core_view_update(g_core_area, g_core_caption));
    CALL_TIME(upd_mem_combo());
    CALL_TIME(upd_net_combo());
    CALL_TIME(upd_disk_combo());
    CALL_TIME(upd_zsum());

    if (g_metrics[13].area != NULL &&
        gtk_widget_get_mapped(g_metrics[13].area))
        CALL_TIME(gui_batt_detail_update());

    if (g_zpool_page.area != NULL)
        queue_draw_maybe(g_zpool_page.area);
    CALL_TIME(upd_zbw());
    CALL_TIME(gui_net_nic_update());
    CALL_TIME(gui_net_health_update());
    CALL_TIME(gui_net_details_update());
    CALL_TIME(gui_disk_details_update());
    CALL_TIME(gui_disk_table_update());
    CALL_TIME(gui_disk_perf_update());
    CALL_TIME(gui_disk_smart_update());
    CALL_TIME(gui_gpu_pages_update());
    CALL_TIME(gui_mem_modules_update());
    CALL_TIME(gui_zpool_detail_update());
    CALL_TIME(gui_arc_detail_update());
    CALL_TIME(gui_zhit_detail_update());
    CALL_TIME(gui_zbw_detail_update());
    CALL_TIME(disk_queue_update());
    CALL_TIME(gui_prune_detail_update());
    CALL_TIME(gui_mem_totals_update());
    CALL_TIME(gui_l2arc_update());
    CALL_TIME(gui_cpu_detail_update());
    CALL_TIME(gui_cpu_gauge_update());
    CALL_TIME(gui_cpu_thermal_update());
    CALL_TIME(gui_cpu_hw_update());

    tm[2] = g_get_monotonic_time();

    warn_if_slow("refresh(total)", t0);

    if (getenv("FMON_REFRESH_STAGE") != NULL) {
        gint64 tot = g_get_monotonic_time() - t0;

        if (tot > 8000)
            g_printerr("refresh-stage metrics=%.2f combos=%.2f details=%.2f "\
                       "total=%.2f ms\n",
                       (double)(tm[1] - tm[0]) / 1000.0,
                       (double)(tm[2] - tm[1]) / 1000.0,
                       (double)(g_get_monotonic_time() - tm[2]) / 1000.0,
                       (double)tot / 1000.0);
    }

    /* FMON_REFRESH_STATS=1：每分钟输出 refresh 总耗时分布，用于调优 */
    refresh_stats_sample(g_get_monotonic_time() - t0);

    if (g_cpu_page.area != NULL)
        queue_draw_maybe(g_cpu_page.area);
    if (g_mem_page.area != NULL)
        queue_draw_maybe(g_mem_page.area);
    if (g_zsum_page.area != NULL)
        queue_draw_maybe(g_zsum_page.area);
    if (g_zbw_page.area != NULL)
        queue_draw_maybe(g_zbw_page.area);
}

void gui_pages_redraw_all(void)
{
    for (int i = 0; i < METRIC_PAGE_COUNT; i++) {
        if (g_metrics[i].area != NULL)
            queue_draw_maybe(g_metrics[i].area);
    }

    if (g_cpu_page.area != NULL)
        queue_draw_maybe(g_cpu_page.area);
    if (g_mem_page.area != NULL)
        queue_draw_maybe(g_mem_page.area);
    if (g_zsum_page.area != NULL)
        queue_draw_maybe(g_zsum_page.area);
    if (g_zbw_page.area != NULL)
        queue_draw_maybe(g_zbw_page.area);
    if (g_core_area != NULL)
        queue_draw_maybe(g_core_area);
}






















