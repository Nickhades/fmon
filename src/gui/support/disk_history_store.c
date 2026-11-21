/*
 * disk_history_store.c - 磁盘历史采样与 SQLite 存取
 */

#include "disk_history.h"
#include "disk_history_internal.h"

#define HIST_KEEP_SEC (7 * 24 * 3600)
#define HIST_WINDOW_SEC (24 * 3600)

#include <glib/gi18n.h>

#include "disk_perf.h"
#include "gui_state.h"

#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

typedef struct {
    char   dev[16];
    double r, w, lat, q, busy, temp;
    int    n, nlat, ntemp;
} Acc;

typedef struct {
    gint64 ts;
    char   dev[16];
    double r, w, lat, q, busy, temp;
    int    has_lat, has_temp;
} HistRow;

typedef struct {
    int      n;
    HistRow *rows;
} Batch;

static Acc  g_acc[FMON_MAX_DISKS];
static int  g_acc_n;
static int  g_samples;

static char *db_path(void)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "fmon", NULL);

    g_mkdir_with_parents(dir, 0700);

    char *p = g_build_filename(dir, "disk_history.db", NULL);

    g_free(dir);
    return p;
}

static gboolean on_sample(gpointer data)
{
    (void)data;

    const FmonShmData *s = gui_state_snapshot();

    if (!gui_state_online())
        return G_SOURCE_CONTINUE;

    const DiskPerf *p = disk_perf_get();
    uint32_t        n = MIN(s->disk_count, (uint32_t)FMON_MAX_DISKS);

    for (uint32_t i = 0; i < n; i++) {
        const FmonDiskInfo *d = &s->disks[i];
        Acc                *a = NULL;

        for (int k = 0; k < g_acc_n; k++)
            if (strcmp(g_acc[k].dev, d->name) == 0) {
                a = &g_acc[k];
                break;
            }

        if (a == NULL) {
            if (g_acc_n >= FMON_MAX_DISKS)
                continue;

            a = &g_acc[g_acc_n++];
            memset(a, 0, sizeof(*a));
            g_strlcpy(a->dev, d->name, sizeof(a->dev));
        }

        a->r += d->read_kbps;
        a->w += d->write_kbps;
        a->busy += d->busy_pct;
        a->n++;

        if (p != NULL && p->valid && strcmp(p->dev, d->name) == 0) {
            a->lat += p->latency_ms;
            a->q += p->queue_len;
            a->nlat++;

            if (d->smart_valid != 0 && d->temp_c > 0.0) {
                a->temp += d->temp_c;
                a->ntemp++;
            }
        }
    }

    g_samples++;
    return G_SOURCE_CONTINUE;
}

static void batch_free(gpointer data)
{
    Batch *b = data;

    if (b != NULL) {
        g_free(b->rows);
        g_free(b);
    }
}

static void write_thread(GTask *task, gpointer src, gpointer data,
                         GCancellable *cancellable)
{
    (void)task;
    (void)src;
    (void)cancellable;

    Batch *b = data;

    if (b == NULL || b->n == 0)
        return;

    char *path = db_path();
    sqlite3 *db = NULL;

    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db != NULL)
            sqlite3_close(db);
        g_free(path);
        return;
    }

    sqlite3_busy_timeout(db, 2000);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS disk_minute ("
        "ts INTEGER NOT NULL, dev TEXT NOT NULL,"
        "read_kbps REAL, write_kbps REAL, latency_ms REAL,"
        "queue REAL, busy_pct REAL, temp_c REAL,"
        "PRIMARY KEY(ts, dev));"
        "CREATE INDEX IF NOT EXISTS idx_disk_minute_ts "
        "ON disk_minute(ts);";

    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        g_free(path);
        return;
    }

    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(
            db,
            "INSERT OR REPLACE INTO disk_minute"
            "(ts,dev,read_kbps,write_kbps,latency_ms,queue,busy_pct,temp_c)"
            "VALUES(?,?,?,?,?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        g_free(path);
        return;
    }

    for (int i = 0; i < b->n; i++) {
        const HistRow *r = &b->rows[i];

        sqlite3_bind_int64(st, 1, r->ts);
        sqlite3_bind_text(st, 2, r->dev, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, r->r);
        sqlite3_bind_double(st, 4, r->w);

        if (r->has_lat)
            sqlite3_bind_double(st, 5, r->lat);
        else
            sqlite3_bind_null(st, 5);

        sqlite3_bind_double(st, 6, r->q);
        sqlite3_bind_double(st, 7, r->busy);

        if (r->has_temp)
            sqlite3_bind_double(st, 8, r->temp);
        else
            sqlite3_bind_null(st, 8);

        sqlite3_step(st);
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }

    sqlite3_finalize(st);

    gint64 cutoff = (gint64)time(NULL) - HIST_KEEP_SEC;

    sqlite3_stmt *del = NULL;

    if (sqlite3_prepare_v2(db, "DELETE FROM disk_minute WHERE ts < ?", -1,
                           &del, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(del, 1, cutoff);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }

    sqlite3_close(db);
    g_free(path);
}

static gboolean on_flush(gpointer data)
{
    (void)data;

    if (g_samples == 0 || g_acc_n == 0)
        return G_SOURCE_CONTINUE;

    Batch *b = g_new0(Batch, 1);

    b->n    = g_acc_n;
    b->rows = g_new0(HistRow, g_acc_n);

    gint64 ts = ((gint64)time(NULL) / 60) * 60;

    for (int i = 0; i < g_acc_n; i++) {
        Acc *a = &g_acc[i];
        HistRow *r = &b->rows[i];
        int n = (a->n > 0) ? a->n : 1;

        r->ts  = ts;
        g_strlcpy(r->dev, a->dev, sizeof(r->dev));
        r->r    = a->r / n;
        r->w    = a->w / n;
        r->busy = a->busy / n;
        r->q    = (a->nlat > 0) ? a->q / a->nlat : 0.0;

        if (a->nlat > 0) {
            r->lat     = a->lat / a->nlat;
            r->has_lat = 1;
        }

        if (a->ntemp > 0) {
            r->temp     = a->temp / a->ntemp;
            r->has_temp = 1;
        }
    }

    memset(g_acc, 0, sizeof(g_acc));
    g_acc_n   = 0;
    g_samples = 0;

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);

    g_task_set_task_data(task, b, batch_free);
    g_task_run_in_thread(task, write_thread);
    g_object_unref(task);

    return G_SOURCE_CONTINUE;
}

void disk_history_start(void)
{
    static gboolean started;

    if (started)
        return;

    started = TRUE;
    g_timeout_add_seconds(10, on_sample, NULL);
    g_timeout_add_seconds(60, on_flush, NULL);
}

GArray *disk_history_query_points(const char *dev, int metric)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(DiskHistoryPoint));
    char   *path = db_path();
    sqlite3 *db = NULL;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db != NULL)
            sqlite3_close(db);
        g_free(path);
        return out;
    }

    const char *sql;

    switch (metric) {
    case 0:
        sql = "SELECT ts,latency_ms FROM disk_minute WHERE dev=?1 AND ts>=?2 "
              "AND latency_ms IS NOT NULL ORDER BY ts";
        break;
    case 2:
        sql = "SELECT ts,temp_c FROM disk_minute WHERE dev=?1 AND ts>=?2 "
              "AND temp_c IS NOT NULL ORDER BY ts";
        break;
    default:
        sql = "SELECT ts,read_kbps+write_kbps FROM disk_minute "
              "WHERE dev=?1 AND ts>=?2 ORDER BY ts";
        break;
    }

    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, dev, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (gint64)time(NULL) - HIST_WINDOW_SEC);

        while (sqlite3_step(st) == SQLITE_ROW) {
            DiskHistoryPoint p;

            p.t = (double)sqlite3_column_int64(st, 0);
            p.v = sqlite3_column_double(st, 1);
            g_array_append_val(out, p);
        }
    }

    sqlite3_finalize(st);
    sqlite3_close(db);
    g_free(path);
    return out;
}

GPtrArray *disk_history_query_devices(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    char      *path = db_path();
    sqlite3   *db = NULL;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db != NULL)
            sqlite3_close(db);
        g_free(path);
        return out;
    }

    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db, "SELECT DISTINCT dev FROM disk_minute "
                               "ORDER BY dev", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out,
                            g_strdup((const char *)sqlite3_column_text(st, 0)));
    }

    sqlite3_finalize(st);
    sqlite3_close(db);
    g_free(path);
    return out;
}
