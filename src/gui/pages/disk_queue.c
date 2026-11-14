/*
 * disk_queue.c - 设备队列长度 / 繁忙度（iostat -x）
 *
 * iostat 的 fork/exec 与 1 秒阻塞放到工作线程，避免占 GTK 主线程。
 */

#include "disk_queue.h"

#include <gio/gio.h>
#include <string.h>

static DiskQueue g_q;
static gint64    g_last_us;
static gboolean  g_running;

static void queue_thread(GTask *task, gpointer source, gpointer data,
                         GCancellable *cancel)
{
    (void)source;
    (void)data;

    GError      *err = NULL;
    GSubprocess *p   = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
                                        "iostat", "-x", "1", "1", NULL);

    if (p == NULL) {
        g_task_return_error(task, err);
        return;
    }

    gchar *out = NULL;

    g_subprocess_communicate_utf8(p, NULL, cancel, &out, NULL, &err);
    g_object_unref(p);

    if (err != NULL) {
        g_free(out);
        g_task_return_error(task, err);
        return;
    }

    g_task_return_pointer(task, out, g_free);
}

static void queue_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    (void)data;

    GError *err = NULL;
    gchar  *out = g_task_propagate_pointer(G_TASK(res), &err);

    if (out != NULL) {
        disk_queue_parse(out, &g_q);
        g_free(out);
    }

    if (err != NULL)
        g_error_free(err);

    g_running = FALSE;
}

void disk_queue_update(void)
{
    if (g_running)
        return;

    gint64 now = g_get_monotonic_time();

    if (g_last_us != 0 && now - g_last_us < 5 * G_TIME_SPAN_SECOND)
        return;

    g_last_us = now;
    g_running = TRUE;

    GTask *task = g_task_new(NULL, NULL, queue_done, NULL);

    g_task_run_in_thread(task, queue_thread);
    g_object_unref(task);
}

const DiskQueue *disk_queue_get(void)
{
    return (g_q.n > 0) ? &g_q : NULL;
}

gboolean disk_queue_parse(const char *text, DiskQueue *out)
{
    if (text == NULL || out == NULL)
        return FALSE;

    out->n = 0;

    char **lines = g_strsplit(text, "\n", -1);
    int    qcol  = -1, bcol = -1;

    for (int i = 0; lines[i] != NULL && out->n < 8; i++) {
        char **tok = g_strsplit_set(lines[i], " \t", -1);
        int    n   = 0;

        for (int k = 0; tok[k] != NULL; k++) {
            if (tok[k][0] != '\0')
                tok[n++] = tok[k];
        }
        tok[n] = NULL;

        if (n == 0) {
            g_strfreev(tok);
            continue;
        }

        if (qcol < 0) {
            for (int k = 0; k < n; k++) {
                if (strcmp(tok[k], "qlen") == 0)
                    qcol = k;
                else if (strcmp(tok[k], "%b") == 0)
                    bcol = k;
            }
            g_strfreev(tok);
            continue;
        }

        if (strcmp(tok[0], "device") == 0 || g_str_has_prefix(tok[0], "pass")) {
            g_strfreev(tok);
            continue;
        }

        if (n > qcol) {
            DiskQueueDev *d = &out->dev[out->n++];

            g_strlcpy(d->name, tok[0], sizeof(d->name));
            d->qlen     = g_ascii_strtod(tok[qcol], NULL);
            d->busy_pct = (bcol >= 0 && n > bcol)
                              ? g_ascii_strtod(tok[bcol], NULL)
                              : 0.0;
        }

        g_strfreev(tok);
    }

    g_strfreev(lines);
    return out->n > 0;
}

