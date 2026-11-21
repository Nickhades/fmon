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

static gboolean g_disk_running;   /* 工作线程采集进行中 */

typedef struct {
    gboolean ok;
    DiskPerf perf;
} DiskSample;

static gboolean is_disk(const struct devstat *d)
{
    static const char *const skip[] = { "pass", "cd", "fd", NULL };

    for (int i = 0; skip[i] != NULL; i++) {
        if (strncmp(d->device_name, skip[i], strlen(skip[i])) == 0)
            return FALSE;
    }
    return TRUE;
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

void disk_perf_collect(DiskPerf *out)
{
    if (out == NULL)
        return;

    DiskSample s;

    memset(&s, 0, sizeof(s));
    disk_collect(&s);

    if (s.ok)
        *out = s.perf;
}

void disk_perf_publish(const DiskPerf *in)
{
    if (in == NULL || !in->valid)
        return;

    g_perf  = *in;
    g_valid = TRUE;
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
    }

    g_free(s);
    g_disk_running = FALSE;
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
