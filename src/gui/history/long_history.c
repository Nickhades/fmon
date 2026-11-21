#include "history/long_history.h"

#include "history/history_provider.h"
#include "gui_page_disk.h"
#include "gui_page_gpu.h"
#include "gui_page_l2arc.h"
#include "gui_page_state.h"
#include "gui_pages.h"
#include "gui_state.h"

#include <string.h>

static bool   s_active;
static gint64 s_range;
static bool   s_disk_temp_bound;
static CurveData s_saved_disk_temp;
static bool   s_gpu_bound[FMON_MAX_GPUS][FMON_GPU_HIST_COUNT];
static CurveData s_saved_gpu[FMON_MAX_GPUS][FMON_GPU_HIST_COUNT];
static bool   s_zpool_bound;
static CurveData s_saved_zpool;
static bool   s_l2_bound[2];
static CurveData s_saved_l2[2];
static bool   s_zfs_perf_bound[3];
static CurveData s_saved_zfs_perf[3];

static void restore_disk_temp(void);
static void restore_gpu_history(void);
static void bind_l2_history(void);
static void restore_l2_history(void);
static void restore_zfs_perf_history(void);

static void bind_series(CurveData *cd, FmonHistoryMetric metric, bool minmax)
{
    const FmonHistorySeries *s = fmon_history_series(metric);

    if (cd == NULL || s == NULL || !s->available) {
        if (cd != NULL) {
            cd->history = NULL;
            cd->count = 0;
            cd->history_min = NULL;
            cd->history_max = NULL;
        }
        return;
    }

    cd->history = s->values;
    cd->history_min = minmax ? s->min : NULL;
    cd->history_max = minmax ? s->max : NULL;
    cd->time_us = fmon_history_time_us();
    cd->count = fmon_history_count();
    cd->end_time_us = (cd->count > 0 && cd->time_us != NULL)
                          ? cd->time_us[cd->count - 1]
                          : 0;
    cd->index = cd->count % FMON_HISTORY_MAX_POINTS;
    cd->interval_us = (uint64_t)fmon_history_interval_s() * 1000000ULL;
    cd->view_start_us = 0;
    cd->view_end_us = 0;
    cd->fixed_range = FALSE;
    cd->frozen = TRUE;
}

static void bind_live(CurveData *cd, const FmonShmData *snap)
{
    if (cd == NULL || snap == NULL)
        return;

    cd->history_min = NULL;
    cd->history_max = NULL;
    cd->view_start_us = 0;
    cd->view_end_us = 0;
    cd->fixed_range = FALSE;
    cd->frozen = FALSE;

    if (cd == &g_net_rx_cd)
        cd->history = snap->net_rx_history;
    else if (cd == &g_net_tx_cd)
        cd->history = snap->net_tx_history;
    else if (cd == &g_disk_rd_cd)
        cd->history = snap->disk_read_history;
    else if (cd == &g_disk_wr_cd)
        cd->history = snap->disk_write_history;

    cd->time_us = snap->history_time_us;
    cd->count = snap->history_count;
    cd->end_time_us = snap->last_update_us;
    cd->index = snap->history_index;
    cd->interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
}

static void bind_disk_temp(void)
{
    const FmonHistorySeries *s =
        fmon_history_series(FMON_HIST_DISK_TEMP);

    if (s == NULL || !s->available) {
        restore_disk_temp();
        return;
    }

    CurveData *dt = gui_disk_temp_curve();

    if (dt == NULL)
        return;

    if (!s_disk_temp_bound) {
        s_saved_disk_temp = *dt;
        s_disk_temp_bound = true;
    }

    dt->history = s->values;
    dt->history_min = NULL;
    dt->history_max = NULL;
    dt->time_us = fmon_history_time_us();
    dt->count = fmon_history_count();
    dt->index = dt->count % FMON_HISTORY_MAX_POINTS;
    dt->interval_us = (uint64_t)fmon_history_interval_s() * 1000000ULL;
    dt->end_time_us = (dt->count > 0 && dt->time_us != NULL)
                          ? dt->time_us[dt->count - 1]
                          : 0;
    dt->view_start_us = 0;
    dt->view_end_us = 0;
    dt->fixed_range = FALSE;
    dt->frozen = TRUE;
}

static void restore_disk_temp(void)
{
    if (!s_disk_temp_bound)
        return;

    CurveData *dt = gui_disk_temp_curve();

    if (dt != NULL)
        *dt = s_saved_disk_temp;

    s_disk_temp_bound = false;
}

static CurveData *gpu_curve_for_metric(int slot, FmonGpuHistoryMetric m)
{
    switch (m) {
    case FMON_GPU_HIST_USAGE:
        return gui_gpu_curve(slot);
    case FMON_GPU_HIST_TEMP:
        return gui_gpu_temp_curve(slot);
    case FMON_GPU_HIST_MEM:
        return gui_gpu_mem_curve(slot);
    default:
        return NULL;
    }
}

static void bind_one_gpu_metric(int slot, FmonGpuHistoryMetric m)
{
    CurveData *cd = gpu_curve_for_metric(slot, m);
    const FmonHistorySeries *s = fmon_gpu_history_series(slot, m);

    if (cd == NULL || s == NULL || !s->available ||
        fmon_gpu_history_count(slot) == 0) {
        if (cd != NULL && s_gpu_bound[slot][m]) {
            *cd = s_saved_gpu[slot][m];
            s_gpu_bound[slot][m] = false;
        }
        return;
    }

    if (!s_gpu_bound[slot][m]) {
        s_saved_gpu[slot][m] = *cd;
        s_gpu_bound[slot][m] = true;
    }

    cd->history = s->values;
    cd->history_min = NULL;
    cd->history_max = NULL;
    cd->time_us = fmon_gpu_history_time_us(slot);
    cd->count = fmon_gpu_history_count(slot);
    cd->index = cd->count % FMON_HISTORY_MAX_POINTS;
    cd->interval_us = 60ULL * 1000000ULL;
    cd->end_time_us = (cd->count > 0 && cd->time_us != NULL)
                          ? cd->time_us[cd->count - 1]
                          : 0;
    cd->view_start_us = 0;
    cd->view_end_us = 0;
    cd->fixed_range = FALSE;
    cd->frozen = TRUE;
}

static void bind_gpu_history(void)
{
    if (!fmon_gpu_history_load(s_range)) {
        restore_gpu_history();
        return;
    }

    for (int slot = 0; slot < FMON_MAX_GPUS; slot++) {
        for (int m = 0; m < FMON_GPU_HIST_COUNT; m++)
            bind_one_gpu_metric(slot, (FmonGpuHistoryMetric)m);
    }
}

static void restore_gpu_history(void)
{
    for (int slot = 0; slot < FMON_MAX_GPUS; slot++) {
        for (int m = 0; m < FMON_GPU_HIST_COUNT; m++) {
            if (!s_gpu_bound[slot][m])
                continue;

            CurveData *cd =
                gpu_curve_for_metric(slot, (FmonGpuHistoryMetric)m);

            if (cd != NULL)
                *cd = s_saved_gpu[slot][m];

            s_gpu_bound[slot][m] = false;
        }
    }

    fmon_gpu_history_unload();
}

static void bind_zfs_perf_metric(int index, FmonZfsHistoryMetric metric)
{
    const FmonHistorySeries *s = fmon_zfs_history_series(metric);
    CurveData *cd = gui_metric_curve(index);

    if (cd == NULL || s == NULL || !s->available ||
        fmon_zfs_history_count() == 0) {
        if (cd != NULL && s_zfs_perf_bound[index]) {
            *cd = s_saved_zfs_perf[index];
            s_zfs_perf_bound[index] = false;
        }
        return;
    }

    if (!s_zfs_perf_bound[index]) {
        s_saved_zfs_perf[index] = *cd;
        s_zfs_perf_bound[index] = true;
    }

    cd->history = s->values;
    cd->history_min = NULL;
    cd->history_max = NULL;
    cd->time_us = fmon_zfs_history_time_us();
    cd->count = fmon_zfs_history_count();
    cd->index = cd->count % FMON_HISTORY_MAX_POINTS;
    cd->interval_us = 60ULL * 1000000ULL;
    cd->end_time_us = (cd->count > 0) ? cd->time_us[cd->count - 1] : 0;
    cd->view_start_us = 0;
    cd->view_end_us = 0;
    cd->fixed_range = FALSE;
    cd->frozen = TRUE;
}

static void bind_zfs_perf_history(void)
{
    bind_zfs_perf_metric(8, FMON_ZFS_HIST_ARC_PCT);
    bind_zfs_perf_metric(9, FMON_ZFS_HIST_READ_KBPS);
    bind_zfs_perf_metric(10, FMON_ZFS_HIST_WRITE_KBPS);
}

static void restore_zfs_perf_history(void)
{
    for (int i = 0; i < 3; i++) {
        if (!s_zfs_perf_bound[i])
            continue;

        CurveData *cd = gui_metric_curve(8 + i);

        if (cd != NULL)
            *cd = s_saved_zfs_perf[i];

        s_zfs_perf_bound[i] = false;
    }
}

static void bind_zfs_history(void)
{
    if (!fmon_zfs_history_load(s_range)) {
        if (s_zpool_bound) {
            g_zpool_cd = s_saved_zpool;
            s_zpool_bound = false;
        }
        restore_l2_history();
        restore_zfs_perf_history();
        return;
    }

    const FmonHistorySeries *s =
        fmon_zfs_history_series(FMON_ZFS_HIST_ZPOOL_UTIL);
    bool zpool_ok = (s != NULL && s->available &&
                     fmon_zfs_history_count() > 0);

    if (zpool_ok) {
        if (!s_zpool_bound) {
            s_saved_zpool = g_zpool_cd;
            s_zpool_bound = true;
        }

        g_zpool_cd.history = s->values;
        g_zpool_cd.history_min = NULL;
        g_zpool_cd.history_max = NULL;
        g_zpool_cd.time_us = fmon_zfs_history_time_us();
        g_zpool_cd.count = fmon_zfs_history_count();
        g_zpool_cd.index = g_zpool_cd.count % FMON_HISTORY_MAX_POINTS;
        g_zpool_cd.interval_us = 60ULL * 1000000ULL;
        g_zpool_cd.end_time_us =
            (g_zpool_cd.count > 0)
                ? g_zpool_cd.time_us[g_zpool_cd.count - 1]
                : 0;
        g_zpool_cd.view_start_us = 0;
        g_zpool_cd.view_end_us = 0;
        g_zpool_cd.fixed_range = FALSE;
        g_zpool_cd.frozen = TRUE;
    } else if (s_zpool_bound) {
        g_zpool_cd = s_saved_zpool;
        s_zpool_bound = false;
    }

    bind_l2_history();
    bind_zfs_perf_history();
}

static void bind_l2_metric(int index, FmonZfsHistoryMetric metric,
                           CurveData *cd)
{
    const FmonHistorySeries *s = fmon_zfs_history_series(metric);

    if (cd == NULL || s == NULL || !s->available ||
        fmon_zfs_history_count() == 0) {
        if (cd != NULL && s_l2_bound[index]) {
            *cd = s_saved_l2[index];
            s_l2_bound[index] = false;
        }
        return;
    }

    if (!s_l2_bound[index]) {
        s_saved_l2[index] = *cd;
        s_l2_bound[index] = true;
    }

    cd->history = s->values;
    cd->history_min = NULL;
    cd->history_max = NULL;
    cd->time_us = fmon_zfs_history_time_us();
    cd->count = fmon_zfs_history_count();
    cd->index = cd->count % FMON_HISTORY_MAX_POINTS;
    cd->interval_us = 60ULL * 1000000ULL;
    cd->end_time_us = (cd->count > 0) ? cd->time_us[cd->count - 1] : 0;
    cd->view_start_us = 0;
    cd->view_end_us = 0;
    cd->fixed_range = FALSE;
    cd->frozen = TRUE;
}

static void bind_l2_history(void)
{
    bind_l2_metric(0, FMON_ZFS_HIST_L2_SIZE, gui_l2arc_size_curve());
    bind_l2_metric(1, FMON_ZFS_HIST_L2_HIT, gui_l2arc_hit_curve());
}

static void restore_l2_history(void)
{
    for (int i = 0; i < 2; i++) {
        if (!s_l2_bound[i])
            continue;

        CurveData *cd = (i == 0) ? gui_l2arc_size_curve()
                                 : gui_l2arc_hit_curve();

        if (cd != NULL)
            *cd = s_saved_l2[i];

        s_l2_bound[i] = false;
    }
}

static void restore_zfs_history(void)
{
    if (s_zpool_bound) {
        g_zpool_cd = s_saved_zpool;
        s_zpool_bound = false;
    }

    fmon_zfs_history_unload();
}

bool gui_long_history_enter(gint64 range_secs)
{
    if (range_secs != 3600 && range_secs != 86400 &&
        range_secs != 7 * 86400 && range_secs != 30 * 86400)
        return false;

    if (!fmon_history_load(range_secs))
        return false;

    CurveData *cpu  = gui_metric_curve(0);
    CurveData *mem  = gui_metric_curve(1);
    CurveData *swap = gui_metric_curve(2);
    CurveData *temp = gui_metric_curve(12);

    if (cpu == NULL || mem == NULL || swap == NULL || temp == NULL) {
        fmon_history_unload();
        return false;
    }

    bind_series(cpu, FMON_HIST_CPU, true);
    bind_series(mem, FMON_HIST_MEM, true);
    bind_series(swap, FMON_HIST_SWAP, false);
    bind_series(temp, FMON_HIST_TEMP, true);

    bind_series(&g_net_rx_cd, FMON_HIST_NET_RX, false);
    bind_series(&g_net_tx_cd, FMON_HIST_NET_TX, false);
    bind_series(&g_disk_rd_cd, FMON_HIST_DISK_RD, false);
    bind_series(&g_disk_wr_cd, FMON_HIST_DISK_WR, false);
    bind_disk_temp();

    s_active = true;
    s_range = range_secs;
    bind_gpu_history();
    bind_zfs_history();
    return true;
}

void gui_long_history_leave(void)
{
    if (!s_active)
        return;

    const FmonShmData *snap = gui_state_snapshot();

    gui_metric_curves_bind(snap);
    bind_live(&g_net_rx_cd, snap);
    bind_live(&g_net_tx_cd, snap);
    bind_live(&g_disk_rd_cd, snap);
    bind_live(&g_disk_wr_cd, snap);
    restore_disk_temp();
    restore_gpu_history();
    restore_l2_history();
    restore_zfs_perf_history();
    restore_zfs_history();

    fmon_history_unload();
    s_active = false;
    s_range = 0;
}

void gui_long_history_refresh(void)
{
    if (!s_active)
        return;

    if (!fmon_history_load(s_range)) {
        gui_long_history_leave();
        return;
    }

    CurveData *cpu  = gui_metric_curve(0);
    CurveData *mem  = gui_metric_curve(1);
    CurveData *swap = gui_metric_curve(2);
    CurveData *temp = gui_metric_curve(12);

    bind_series(cpu, FMON_HIST_CPU, true);
    bind_series(mem, FMON_HIST_MEM, true);
    bind_series(swap, FMON_HIST_SWAP, false);
    bind_series(temp, FMON_HIST_TEMP, true);
    bind_series(&g_net_rx_cd, FMON_HIST_NET_RX, false);
    bind_series(&g_net_tx_cd, FMON_HIST_NET_TX, false);
    bind_series(&g_disk_rd_cd, FMON_HIST_DISK_RD, false);
    bind_series(&g_disk_wr_cd, FMON_HIST_DISK_WR, false);
    bind_disk_temp();
    bind_gpu_history();
    bind_zfs_history();
}

bool gui_long_history_active(void)
{
    return s_active;
}

gint64 gui_long_history_range(void)
{
    return s_range;
}
