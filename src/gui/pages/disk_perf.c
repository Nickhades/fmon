/*
 * disk_perf.c - devstat 磁盘性能采样
 */

#include "disk_perf.h"

#include <gio/gio.h>

#include <devstat.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devicestat.h>

static DiskPerf      g_perf;
static gboolean      g_valid = FALSE;
static struct devstat g_prev;
static gboolean      g_have_prev = FALSE;
static long double   g_prev_time = 0.0;
static struct devinfo *g_dinfo;   /* devstat 需要调用方先分配 dinfo */

static gint64   g_smart_ms;
static gboolean g_smart_running;
static char     g_smart_dev[16];
static gboolean g_disk_running;   /* 工作线程采集进行中 */

typedef struct {
    gboolean ok;
    DiskPerf perf;
    char     smart_dev[16];
} DiskSample;

/* 解析 "Temperature: 315 K, 41.85 C" / "Available spare: 100" /
 * "Percentage used: 6"（nvmecontrol logpage -p 2） */
gboolean disk_perf_parse_smart(const char *text, double *temp_c,
                               int *spare_pct, int *wear_pct)
{
    if (text == NULL)
        return FALSE;

    gboolean got = FALSE;
    char   **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        const char *l = lines[i];

        while (*l == ' ' || *l == '\t')
            l++;

        const char *colon = strchr(l, ':');

        if (colon == NULL)
            continue;

        if (g_str_has_prefix(l, "Temperature:")) {
            /* "Temperature: 315 K, 41.85 C, 107.33 F" */
            const char *c = strstr(colon, " K, ");

            if (c != NULL && temp_c != NULL) {
                c += 4;
                *temp_c = g_ascii_strtod(c, NULL);
                got = TRUE;
            }
        } else if (g_str_has_prefix(l, "Available spare:")) {
            if (spare_pct != NULL)
                *spare_pct = atoi(colon + 1);
            got = TRUE;
        } else if (g_str_has_prefix(l, "Percentage used:")) {
            if (wear_pct != NULL)
                *wear_pct = atoi(colon + 1);
            got = TRUE;
        }
    }

    g_strfreev(lines);
    return got;
}

static gboolean is_disk(const struct devstat *d)
{
    static const char *const skip[] = { "pass", "cd", "fd", NULL };

    for (int i = 0; skip[i] != NULL; i++) {
        if (strncmp(d->device_name, skip[i], strlen(skip[i])) == 0)
            return FALSE;
    }
    return TRUE;
}

/* ===========================================================================
 * NVMe SMART：每 30 秒异步跑一次 nvmecontrol（普通用户可读 /dev/nvme0）
 * ========================================================================= */
static double  g_temp_c;
static int     g_spare_pct;
static int     g_wear_pct;
static gboolean g_has_smart;

static void on_smart(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GError *err = NULL;
    gchar  *out = NULL;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL,
                                         &err);
    if (err != NULL)
        g_error_free(err);

    double t = 0.0;
    int    s = 0, w = 0;

    if (out != NULL && disk_perf_parse_smart(out, &t, &s, &w)) {
        g_temp_c    = t;
        g_spare_pct = s;
        g_wear_pct  = w;
        g_has_smart = TRUE;
    }

    g_free(out);
    g_object_unref(src);
    g_smart_running = FALSE;
}

static void smart_poll(void)
{
    if (g_smart_running || g_smart_dev[0] == '\0')
        return;

    gint64 now = g_get_monotonic_time() / 1000;

    if (g_smart_ms != 0 && now - g_smart_ms < 30000)
        return;

    g_smart_ms = now;

    GError      *err = NULL;
    GSubprocess *p   = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err, "nvmecontrol",
        "logpage", "-p", "2", g_smart_dev, NULL);

    if (p == NULL) {
        if (err != NULL)
            g_error_free(err);
        return;
    }

    g_smart_running = TRUE;
    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_smart, NULL);
}

static void disk_collect(DiskSample *sample)
{
    struct statinfo sti;

    memset(&sti, 0, sizeof(sti));

    if (g_dinfo == NULL)
        g_dinfo = calloc(1, sizeof(*g_dinfo));

    sti.dinfo = g_dinfo;

    if (devstat_getdevs(NULL, &sti) < 0 || sti.dinfo == NULL)
        return;

    struct devstat *pick = NULL;

    for (int i = 0; i < sti.dinfo->numdevs; i++) {
        struct devstat *d = &sti.dinfo->devices[i];

        if (!is_disk(d))
            continue;

        if (pick == NULL)
            pick = d;

        if (strncmp(d->device_name, "nda", 3) == 0 ||
            strncmp(d->device_name, "ada", 3) == 0) {
            pick = d;
            break;
        }
    }

    if (pick == NULL)
        return;

    char name[24];

    g_snprintf(name, sizeof(name), "%s%d", pick->device_name,
               pick->unit_number);

    DiskPerf p;

    memset(&p, 0, sizeof(p));
    p.valid = TRUE;
    g_strlcpy(p.dev, name, sizeof(p.dev));

    if (g_have_prev && g_strcmp0(g_perf.dev, name) == 0) {
        long double etime = sti.snap_time - g_prev_time;

        if (etime > 0.0) {
            long double rps = 0, wps = 0, ms = 0, busy = 0, qlen = 0;

            devstat_compute_statistics(
                pick, &g_prev, etime,
                DSM_TRANSFERS_PER_SECOND_READ, &rps,
                DSM_TRANSFERS_PER_SECOND_WRITE, &wps,
                DSM_MS_PER_TRANSACTION, &ms,
                DSM_BUSY_PCT, &busy,
                DSM_QUEUE_LENGTH, &qlen,
                DSM_NONE);

            p.read_iops  = (double)rps;
            p.write_iops = (double)wps;
            p.latency_ms = (double)ms;
            p.busy_pct   = (double)busy;
            p.queue_len  = (double)qlen;
        }
    }

    g_prev      = *pick;
    g_prev_time = sti.snap_time;
    g_have_prev = TRUE;

    p.total_read_bytes  = pick->bytes[DEVSTAT_READ];
    p.total_write_bytes = pick->bytes[DEVSTAT_WRITE];

    g_snprintf(sample->smart_dev, sizeof(sample->smart_dev), "nvme%d",
               pick->unit_number);

    p.has_smart = g_has_smart;
    p.temp_c    = g_temp_c;
    p.spare_pct = g_spare_pct;
    p.wear_pct  = g_wear_pct;

    sample->perf = p;
    sample->ok   = TRUE;
}

static void disk_task_thread(GTask *task, gpointer src, gpointer data,
                             GCancellable *cancellable)
{
    (void)src;
    (void)data;
    (void)cancellable;

    DiskSample *s = g_new0(DiskSample, 1);

    disk_collect(s);
    g_task_return_pointer(task, s, g_free);
}

void disk_perf_collect(DiskPerf *out, char smart_dev[16])
{
    if (out == NULL || smart_dev == NULL)
        return;

    DiskSample s;

    memset(&s, 0, sizeof(s));
    disk_collect(&s);

    if (s.ok) {
        *out = s.perf;
        g_strlcpy(smart_dev, s.smart_dev, 16);
    }
}

void disk_perf_publish(const DiskPerf *in, const char *smart_dev)
{
    if (in == NULL || !in->valid)
        return;

    g_perf  = *in;
    g_valid = TRUE;

    if (smart_dev != NULL)
        g_strlcpy(g_smart_dev, smart_dev, sizeof(g_smart_dev));

    smart_poll();
}

static void disk_task_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    (void)data;

    GTask      *task = G_TASK(res);
    DiskSample *s    = g_task_propagate_pointer(task, NULL);

    if (s != NULL && s->ok) {
        g_perf  = s->perf;
        g_valid = TRUE;
        g_strlcpy(g_smart_dev, s->smart_dev, sizeof(g_smart_dev));
    }

    g_free(s);
    g_disk_running = FALSE;

    if (g_valid)
        smart_poll();
}

void disk_perf_update(void)
{
    if (g_disk_running)
        return;

    g_disk_running = TRUE;

    GTask *task = g_task_new(NULL, NULL, disk_task_done, NULL);

    g_task_run_in_thread(task, disk_task_thread);
    g_object_unref(task);
}

const DiskPerf *disk_perf_get(void)
{
    return g_valid ? &g_perf : NULL;
}


