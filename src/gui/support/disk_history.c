/*
 * disk_history.c - 磁盘 24 小时历史（SQLite，每分钟聚合，保留 7 天）
 *
 * 采样：每 10 秒累加，每 60 秒把上一分钟的平均值交给后台线程写库。
 * 写库线程独立打开连接，主循环不做任何 SQLite I/O。
 */

#include "disk_history.h"

#include "disk_history_internal.h"

#include <glib/gi18n.h>

#include "disk_perf.h"
#include "gui_state.h"
#include "theme.h"

#include <math.h>
#include <sqlite3.h>
#include <time.h>

#define HIST_KEEP_SEC (7 * 24 * 3600)
#define HIST_WINDOW_SEC (24 * 3600)

/* ===========================================================================
 * 24 小时历史窗口
 * ========================================================================= */




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

    GArray *pts = disk_history_query_points(ctx->dev, ctx->metric);
    GString *s = g_string_new("time,value,unit\n");

    for (guint i = 0; i < pts->len; i++) {
        DiskHistoryPoint p = g_array_index(pts, DiskHistoryPoint, i);
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

    GArray *pts = disk_history_query_points(d->dev, metric);

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
        DiskHistoryPoint p = g_array_index(pts, DiskHistoryPoint, i);
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

    GPtrArray   *devs = disk_history_query_devices();
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
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->area), disk_history_draw, d,
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
