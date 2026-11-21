/*
 * collector_thread.c - GUI 常驻采集线程
 */

#include "collector_thread.h"

#include "cpu_stats.h"
#include "disk_perf.h"
#include "net_stats.h"
#include "zfs_stats.h"

#include <stdlib.h>

typedef struct {
    ZfsStats zfs;
    CpuStats cpu;
    NetStats net;
    DiskPerf disk;
} Frame;

static GThread *g_thread;
static gint     g_running;
static gint     g_pending;   /* 已派发未发布的帧数 */
static gboolean g_ready_sent;
static GuiCollectorReadyFn g_ready_cb;

void gui_collector_set_ready_cb(GuiCollectorReadyFn fn)
{
    g_ready_cb = fn;
}

static gboolean publish_frame(gpointer data)
{
    Frame *f = data;

    /* 整帧发布：同一 tick 的数据一起切换，避免页面上半新半旧 */
    zfs_stats_publish(&f->zfs);
    cpu_stats_publish(&f->cpu);
    net_stats_publish(&f->net);
    disk_perf_publish(&f->disk);

    g_atomic_int_dec_and_test(&g_pending);
    g_free(f);

    if (!g_ready_sent) {
        g_ready_sent = TRUE;

        if (g_ready_cb != NULL)
            g_ready_cb();
    }

    return G_SOURCE_REMOVE;
}

static gpointer collector_main(gpointer data)
{
    (void)data;

    gint64 next = g_get_monotonic_time();

    while (g_atomic_int_get(&g_running)) {
        Frame *f = g_new0(Frame, 1);

        /* 工作线程只读系统信息，不碰 GTK/共享内存发布 */
        zfs_stats_collect(&f->zfs);
        cpu_stats_collect(&f->cpu);
        net_stats_collect_raw(&f->net);
        disk_perf_collect(&f->disk);

        /* 主循环繁忙时丢旧帧，最多保留 2 帧待发布 */
        if (g_atomic_int_get(&g_pending) < 2) {
            g_atomic_int_inc(&g_pending);
            g_idle_add(publish_frame, f);
        } else {
            g_free(f);
        }

        next += G_TIME_SPAN_SECOND;

        /* 分段睡眠：关闭窗口时 gui_collector_stop() 最多等 50ms，
         * 而不是等完整 1 秒采样周期。 */
        while (g_atomic_int_get(&g_running)) {
            gint64 now = g_get_monotonic_time();

            if (now >= next)
                break;

            gint64 remain = next - now;
            gint64 slice = MIN(remain, 50 * G_TIME_SPAN_MILLISECOND);

            g_usleep((gulong)slice);
        }

        if (!g_atomic_int_get(&g_running))
            break;

        gint64 now = g_get_monotonic_time();

        if (next < now)
            next = now;
    }

    return NULL;
}

gboolean gui_collector_active(void)
{
    return g_thread != NULL;
}

void gui_collector_start(void)
{
    if (g_thread != NULL)
        return;

    if (getenv("FMON_LEGACY_COLLECT") != NULL)
        return;

    g_atomic_int_set(&g_running, 1);
    g_thread = g_thread_new("fmon-collect", collector_main, NULL);
}

void gui_collector_stop(void)
{
    if (g_thread == NULL)
        return;

    g_atomic_int_set(&g_running, 0);
    g_thread_join(g_thread);
    g_thread = NULL;
}
