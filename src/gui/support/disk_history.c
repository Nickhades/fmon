/*
 * disk_history.c - 磁盘 24 小时历史（SQLite，每分钟聚合，保留 7 天）
 *
 * 采样：每 10 秒累加，每 60 秒把上一分钟的平均值交给后台线程写库。
 * 写库线程独立打开连接，主循环不做任何 SQLite I/O。
 */

#include "disk_history.h"

#include <glib/gi18n.h>

#include "disk_perf.h"
#include "gui_state.h"
#include "theme.h"

#include <math.h>
#include <sqlite3.h>
#include <time.h>

#define HIST_KEEP_SEC (7 * 24 * 3600)
#define HIST_WINDOW_SEC (24 * 3600)

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

/* ===========================================================================
 * 24 小时历史窗口
 * ========================================================================= */
typedef struct {
    double t;   /* unix 秒 */
    double v;
} Pt;

typedef struct {
    char       dev[16];
    GtkWidget *area;
    GtkWidget *drop;
    GtkWidget *devdrop;
    GtkWidget *stats;

    GArray    *pts;       /* 当前绘图点，供悬停查找 */
    double     ml, mt, pw, ph, since, window, vmin, vmax;
    double     hover_x;
    gboolean   hover;
} HistDlg;

static GArray *query_points(const char *dev, int metric)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(Pt));
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
            Pt p;

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

static GPtrArray *query_devices(void)
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

static void on_device_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj;
    (void)pspec;

    HistDlg *d = data;
    guint    sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->devdrop));
    GtkStringList *sl = GTK_STRING_LIST(
        gtk_drop_down_get_model(GTK_DROP_DOWN(d->devdrop)));
    const char *s = gtk_string_list_get_string(sl, sel);

    if (s != NULL && s[0] != '\0') {
        g_strlcpy(d->dev, s, sizeof(d->dev));
        gtk_widget_queue_draw(d->area);
    }
}

static void draw_history(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                         gpointer data)
{
    (void)area;

    HistDlg *d = data;

    int metric = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->drop));

    if (metric < 0)
        metric = 0;

    if (d->pts != NULL) {
        g_array_free(d->pts, TRUE);
        d->pts = NULL;
    }

    GArray *pts = query_points(d->dev, metric);

    d->pts = pts;

    double axis[4], grid[4];

    fmon_theme_axis_color(axis);
    fmon_theme_grid_color(grid, FALSE);

    double ml = 54.0, mr = 14.0, mt = 26.0, mb = 30.0;
    double pw = w - ml - mr;
    double ph = h - mt - mb;

    if (pw < 10.0 || ph < 10.0)
        return;

    d->ml = ml;
    d->mt = mt;
    d->pw = pw;
    d->ph = ph;

    gint64 now   = (gint64)time(NULL);
    gint64 since = now - HIST_WINDOW_SEC;

    /* 历史还不足一天时自动放大到已有数据，否则几个点会挤在右边缘 */
    if (pts->len > 0) {
        gint64 first = (gint64)g_array_index(pts, Pt, 0).t;

        if (first > since + 6 * 3600)
            since = first - 600;
    }

    double window = (double)(now - since);

    double r = 0.30, g = 0.76, b = 1.00;
    const char *unit = "KB/s";

    if (metric == 0) {
        r = 0.95; g = 0.60; b = 0.20; unit = "ms";
    } else if (metric == 2) {
        r = 0.95; g = 0.30; b = 0.30; unit = "°C";
    }

    cairo_set_line_width(cr, 1.0);

    for (int i = 0; i <= 4; i++) {
        double y = mt + ph * (double)i / 4.0;

        cairo_set_source_rgba(cr, grid[0], grid[1], grid[2], grid[3]);
        cairo_move_to(cr, ml, y);
        cairo_line_to(cr, ml + pw, y);
        cairo_stroke(cr);
    }

    if (pts->len == 0) {
        if (d->stats != NULL)
            gtk_label_set_text(GTK_LABEL(d->stats),
                               _("暂无明细数据（运行 1 分钟后开始记录）"));

        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], axis[3]);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, ml + 12.0, mt + ph / 2.0);
        cairo_show_text(cr, "暂无历史数据（运行 1 分钟后开始记录）");
        return;
    }

    double vmin = pts->len ? g_array_index(pts, Pt, 0).v : 0.0;
    double vmax = vmin;

    for (guint i = 1; i < pts->len; i++) {
        double v = g_array_index(pts, Pt, i).v;

        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    if (vmax - vmin < 1e-9) {
        vmin -= 1.0;
        vmax += 1.0;
    } else {
        double pad = (vmax - vmin) * 0.10;

        vmin -= pad;
        vmax += pad;
    }

    if (vmin < 0.0)
        vmin = 0.0;

    d->since  = since;
    d->window = window;
    d->vmin   = vmin;
    d->vmax   = vmax;

    /* 明细统计：最新 / 最小 / 最大 / 平均 / 点数 */
    {
        double sum = 0.0, lo = 1e18, hi = -1e18;

        for (guint i = 0; i < pts->len; i++) {
            double v = g_array_index(pts, Pt, i).v;

            sum += v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }

        double latest = g_array_index(pts, Pt, pts->len - 1).v;
        gchar *st = g_strdup_printf(
            _("最新 %.2f %s · 最小 %.2f · 最大 %.2f · 平均 %.2f · %u 个采样点"),
            latest, unit, lo, hi, sum / (double)pts->len, pts->len);

        if (d->stats != NULL)
            gtk_label_set_text(GTK_LABEL(d->stats), st);
        g_free(st);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);

    for (int i = 0; i <= 4; i++) {
        double v = vmax - (vmax - vmin) * (double)i / 4.0;
        char   buf[32];

        g_snprintf(buf, sizeof(buf), "%.2f %s", v, unit);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.85);
        cairo_text_extents_t ext;

        cairo_text_extents(cr, buf, &ext);
        cairo_move_to(cr, ml - 6.0 - ext.width, mt + ph * (double)i / 4.0 + 3.0);
        cairo_show_text(cr, buf);
    }

    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.85);
    char left[32];

    if (window >= 20 * 3600)
        g_strlcpy(left, "24 小时前", sizeof(left));
    else if (window >= 3600)
        g_snprintf(left, sizeof(left), "%.0f 小时前", window / 3600.0);
    else
        g_snprintf(left, sizeof(left), "%.0f 分钟前", window / 60.0);

    cairo_move_to(cr, ml, mt + ph + 18.0);
    cairo_show_text(cr, left);
    cairo_move_to(cr, ml + pw - 32.0, mt + ph + 18.0);
    cairo_show_text(cr, "现在");

    cairo_set_source_rgba(cr, r, g, b, 1.0);
    cairo_set_line_width(cr, 1.8);

    gboolean first = TRUE;

    for (guint i = 0; i < pts->len; i++) {
        Pt p = g_array_index(pts, Pt, i);
        double x = ml + pw * (p.t - (double)since) / window;
        double y = mt + ph - ph * (p.v - vmin) / (vmax - vmin);

        if (x < ml) x = ml;
        if (x > ml + pw) x = ml + pw;
        if (y < mt) y = mt;
        if (y > mt + ph) y = mt + ph;

        if (first) {
            cairo_move_to(cr, x, y);
            first = FALSE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }

    cairo_stroke(cr);

    /* 只有一个采样点时画一个圆点，否则什么都看不到 */
    if (pts->len == 1) {
        Pt p = g_array_index(pts, Pt, 0);
        double x = ml + pw * (p.t - (double)since) / window;
        double y = mt + ph - ph * (p.v - vmin) / (vmax - vmin);

        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_arc(cr, x, y, 3.0, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    /* 悬停：显示最近采样点的时间与值 */
    if (d->hover && pts->len > 0) {
        double best = 1e18;
        guint  bi   = 0;

        for (guint i = 0; i < pts->len; i++) {
            Pt p = g_array_index(pts, Pt, i);
            double x = ml + pw * (p.t - (double)since) / window;
            double dd = fabs(x - d->hover_x);

            if (dd < best) {
                best = dd;
                bi   = i;
            }
        }

        Pt p = g_array_index(pts, Pt, bi);
        double x = ml + pw * (p.t - (double)since) / window;
        double y = mt + ph - ph * (p.v - vmin) / (vmax - vmin);

        if (x < ml) x = ml;
        if (x > ml + pw) x = ml + pw;
        if (y < mt) y = mt;
        if (y > mt + ph) y = mt + ph;

        cairo_save(cr);
        double dashes[] = { 3.0, 3.0 };

        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.55);
        cairo_move_to(cr, x, mt);
        cairo_line_to(cr, x, mt + ph);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);

        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_arc(cr, x, y, 3.5, 0, 2 * M_PI);
        cairo_fill(cr);

        struct tm tmv;
        time_t    tt = (time_t)p.t;

        localtime_r(&tt, &tmv);

        char hhmm[16], txt[72];

        strftime(hhmm, sizeof(hhmm), "%H:%M", &tmv);
        g_snprintf(txt, sizeof(txt), "%s  %.2f %s", hhmm, p.v, unit);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, txt, &ext);

        double tx = x + 8.0;

        if (tx + ext.width > ml + pw)
            tx = x - 8.0 - ext.width;

        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.95);
        cairo_move_to(cr, tx, mt + 14.0);
        cairo_show_text(cr, txt);
        cairo_restore(cr);
    }

    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.7);
    cairo_set_font_size(cr, 9.0);
    cairo_move_to(cr, ml + 4.0, mt - 8.0);
    cairo_show_text(cr, "每分钟平均 · 保留 7 天");
}

typedef struct {
    char dev[16];
    int  metric;
} ExportCtx;

static void export_ctx_free(gpointer data)
{
    g_free(data);
}

static const char *metric_unit(int metric)
{
    return (metric == 0) ? "ms" : (metric == 2) ? "C" : "KB/s";
}

static void on_save_done(GObject *src, GAsyncResult *res, gpointer data)
{
    ExportCtx *ctx = data;
    GError    *err = NULL;
    GFile     *file =
        gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);

    if (file == NULL) {
        if (err != NULL)
            g_error_free(err);
        export_ctx_free(ctx);
        return;
    }

    GArray *pts = query_points(ctx->dev, ctx->metric);
    GString *s = g_string_new("time,value,unit\n");

    for (guint i = 0; i < pts->len; i++) {
        Pt p = g_array_index(pts, Pt, i);
        time_t tt = (time_t)p.t;
        struct tm tmv;
        char tbuf[32];

        localtime_r(&tt, &tmv);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);
        g_string_append_printf(s, "%s,%.3f,%s\n", tbuf, p.v,
                               metric_unit(ctx->metric));
    }

    g_file_replace_contents(file, s->str, s->len, NULL, FALSE,
                            G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL,
                            &err);

    if (err != NULL)
        g_error_free(err);

    g_string_free(s, TRUE);
    g_array_free(pts, TRUE);
    g_object_unref(file);
    export_ctx_free(ctx);
}

static void on_export_csv(GtkButton *btn, gpointer data)
{
    (void)btn;

    HistDlg *d = data;
    int metric = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->drop));

    if (metric < 0)
        metric = 0;

    ExportCtx *ctx = g_new0(ExportCtx, 1);

    g_strlcpy(ctx->dev, d->dev, sizeof(ctx->dev));
    ctx->metric = metric;

    GtkFileDialog *fd = gtk_file_dialog_new();

    gtk_file_dialog_set_initial_name(fd, "fmon-disk-history.csv");
    gtk_file_dialog_save(fd, GTK_WINDOW(gtk_widget_get_root(d->area)), NULL,
                         on_save_done, ctx);
    g_object_unref(fd);
}

static void on_show_list(GtkButton *btn, gpointer data)
{
    (void)btn;

    HistDlg *d = data;
    int metric = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->drop));

    if (metric < 0)
        metric = 0;

    GArray *pts = query_points(d->dev, metric);

    GtkWidget *win = gtk_window_new();
    char title[96];

    g_snprintf(title, sizeof(title), _("历史明细 · %s"), d->dev);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 480);
    gtk_window_set_transient_for(GTK_WINDOW(win),
                                 GTK_WINDOW(gtk_widget_get_root(d->area)));

    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_list_box_new();

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);

    for (guint i = 0; i < pts->len; i++) {
        Pt p = g_array_index(pts, Pt, i);
        time_t tt = (time_t)p.t;
        struct tm tmv;
        char tbuf[32], line[96];

        localtime_r(&tt, &tmv);
        strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M", &tmv);
        g_snprintf(line, sizeof(line), "%s\t%.3f %s", tbuf, p.v,
                   metric_unit(metric));

        GtkWidget *lb = gtk_label_new(line);

        gtk_label_set_xalign(GTK_LABEL(lb), 0.0f);
        gtk_widget_set_margin_start(lb, 8);
        gtk_widget_set_margin_end(lb, 8);
        gtk_widget_set_margin_top(lb, 3);
        gtk_widget_set_margin_bottom(lb, 3);
        gtk_list_box_append(GTK_LIST_BOX(list), lb);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_window_set_child(GTK_WINDOW(win), scroll);
    gtk_window_present(GTK_WINDOW(win));

    g_array_free(pts, TRUE);
}

static void on_motion(GtkEventControllerMotion *ctrl, double x, double y,
                      gpointer data)
{
    (void)ctrl;
    (void)y;

    HistDlg *d = data;

    d->hover_x = x;
    d->hover   = TRUE;
    gtk_widget_queue_draw(d->area);
}

static void on_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    (void)ctrl;

    HistDlg *d = data;

    d->hover = FALSE;
    gtk_widget_queue_draw(d->area);
}

static void on_metric_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj;
    (void)pspec;

    HistDlg *d = data;

    gtk_widget_queue_draw(d->area);
}

void disk_history_open_dialog(const char *dev, GtkWindow *parent)
{
    HistDlg *d = g_new0(HistDlg, 1);

    g_strlcpy(d->dev, dev, sizeof(d->dev));

    GtkWidget *win = gtk_window_new();
    char title[64];

    g_snprintf(title, sizeof(title), _("设备历史 · %s"), dev);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 420);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *devlab = gtk_label_new(_("设备"));

    gtk_box_append(GTK_BOX(top), devlab);

    GPtrArray   *devs = query_devices();
    GtkStringList *sl = gtk_string_list_new(NULL);
    int            cur = -1;

    for (guint i = 0; i < devs->len; i++) {
        const char *nm = g_ptr_array_index(devs, i);

        gtk_string_list_append(sl, nm);
        if (strcmp(nm, dev) == 0)
            cur = (int)i;
    }

    if (cur < 0) {
        gtk_string_list_append(sl, dev);
        cur = (int)devs->len;
    }

    /* gtk_drop_down_new() 的 model 是 transfer full：所有权交给下拉框，
     * 这里不能再 unref，也不能再挂到窗口 data 上，否则关闭窗口时 double free。 */
    d->devdrop = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);

    if (cur >= 0)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(d->devdrop), (guint)cur);

    g_ptr_array_free(devs, TRUE);
    gtk_box_append(GTK_BOX(top), d->devdrop);

    GtkWidget *lab = gtk_label_new(_("指标"));
    gtk_box_append(GTK_BOX(top), lab);

    static const char *const metrics[] = { "延迟 (ms)", "吞吐 (KB/s)",
                                           "温度 (°C)", NULL };

    d->drop = gtk_drop_down_new_from_strings(metrics);
    gtk_box_append(GTK_BOX(top), d->drop);

    GtkWidget *btn_list = gtk_button_new_with_label(_("完整列表"));
    GtkWidget *btn_csv  = gtk_button_new_with_label(_("导出 CSV"));

    g_signal_connect(btn_list, "clicked", G_CALLBACK(on_show_list), d);
    g_signal_connect(btn_csv, "clicked", G_CALLBACK(on_export_csv), d);
    gtk_box_append(GTK_BOX(top), btn_list);
    gtk_box_append(GTK_BOX(top), btn_csv);

    gtk_box_append(GTK_BOX(vbox), top);

    d->stats = gtk_label_new("");
    gtk_widget_add_css_class(d->stats, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(d->stats), 0.0f);
    gtk_box_append(GTK_BOX(vbox), d->stats);

    d->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(d->area, TRUE);
    gtk_widget_set_vexpand(d->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->area), draw_history, d,
                                   NULL);
    gtk_box_append(GTK_BOX(vbox), d->area);

    GtkEventController *motion = gtk_event_controller_motion_new();

    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), d);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), d);
    gtk_widget_add_controller(d->area, motion);

    gtk_window_set_child(GTK_WINDOW(win), vbox);

    g_signal_connect(d->drop, "notify::selected",
                     G_CALLBACK(on_metric_changed), d);
    g_signal_connect(d->devdrop, "notify::selected",
                     G_CALLBACK(on_device_changed), d);

    g_object_set_data_full(G_OBJECT(win), "hist-dlg", d, g_free);

    gtk_window_present(GTK_WINDOW(win));
}
