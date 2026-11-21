/*
 * gui_snapshot.c - 共享内存快照、自动重连、刷新定时器与退出收尾
 */

#include "gui_runtime.h"

#include "gui_runtime_internal.h"
#include "gui_state.h"

#include <glib.h>
#include <gtk/gtk.h>

#include <errno.h>
#include <string.h>
#include <sys/types.h>

FmonShmData  g_snap;
int           g_gpu_index = 0;
guint         g_ui_timer = 0;
FmonShmData  *g_live = NULL;
gboolean      g_online = FALSE;
gboolean      g_ever_linked = FALSE;
gchar        *g_link_error = NULL;
gchar        *g_alert_summary = NULL;
GtkWidget    *g_alert_label = NULL;
GtkWidget    *g_header_sub = NULL;
GtkWidget    *g_banner = NULL;
GtkWidget    *g_banner_label = NULL;


const FmonShmData *gui_state_snapshot(void)
{
    return &g_snap;
}

gboolean gui_state_online(void)
{
    return g_online;
}

int gui_state_gpu_index(void)
{
    return g_gpu_index;
}

void gui_state_set_gpu_index(int index)
{
    g_gpu_index = index;
}

uint64_t gui_monotonic_us(void)
{
    return (uint64_t)g_get_monotonic_time();
}

static void set_link_error(const char *msg)
{
    g_free(g_link_error);
    g_link_error = g_strdup(msg);
}

void gui_runtime_attach(void)
{
    if (g_live != NULL)
        return;

    char err[256];
    FmonShmData *p = fmon_shm_attach(false, err, sizeof(err));

    if (p == NULL) {
        set_link_error(err);
        return;
    }

    g_live        = p;
    g_ever_linked = TRUE;
    set_link_error(NULL);
}

/* 取一份一致快照 */
void gui_runtime_snapshot(void)
{
    if (g_live == NULL)
        gui_runtime_attach();

    if (g_live == NULL) {
        memset(&g_snap, 0, sizeof(g_snap));
        g_online = FALSE;
        return;
    }

    if (fmon_shm_snapshot(g_live, &g_snap) != 0) {
        /* 瞬时冲突（采集线程正持锁写入）不等于守护进程挂起：
         * 保持上一次的在线状态和快照，避免横幅闪烁。 */
        return;
    }

    g_online = fmon_shm_is_online(&g_snap, gui_monotonic_us(),
                                  (uint64_t)FMON_STALE_AFTER_MS * 1000u);

    /* 数据持续过期：可能是守护进程重启后换了共享内存对象（旧映射永远
     * 不会再更新）。每隔 8 秒重新 attach 一次，实现自动重连。 */
    static int64_t last_reattach_us = 0;

    if (!g_online) {
        int64_t now = (int64_t)gui_monotonic_us();

        if (now - last_reattach_us > 8 * 1000 * 1000) {
            last_reattach_us = now;

            fmon_shm_detach(g_live);
            g_live = NULL;

            gui_runtime_attach();   /* 成功则下一轮 snapshot 就恢复 */

            if (g_live != NULL &&
                fmon_shm_snapshot(g_live, &g_snap) == 0)
                g_online = fmon_shm_is_online(&g_snap, gui_monotonic_us(),
                                              (uint64_t)FMON_STALE_AFTER_MS *
                                                  1000u);
        }
    }
}

/* ===========================================================================
 * 横幅 / 页头
 * ========================================================================= */
/* 右上角告警提示：按当前阈值检查快照并显示摘要 */
void gui_runtime_start_timer(GSourceFunc fn)
{
    if (g_ui_timer == 0) {
        guint interval = 1000;
        const char *env = getenv("FMON_REFRESH_MS");

        if (env != NULL) {
            int v = atoi(env);

            if (v >= 200 && v <= 10000)
                interval = (guint)v;
        }

        g_ui_timer = g_timeout_add(interval, fn, NULL);
    }
}

void gui_runtime_stop_timer(void)
{
    if (g_ui_timer != 0) {
        g_source_remove(g_ui_timer);
        g_ui_timer = 0;
    }
}

void gui_runtime_shutdown(void)
{
    gui_runtime_stop_timer();

    if (g_live != NULL) {
        fmon_shm_detach(g_live);
        g_live = NULL;
    }

    g_free(g_alert_summary);
    g_alert_summary = NULL;
}
