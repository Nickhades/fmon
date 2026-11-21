/*
 * history_export.c - 历史数据 CSV 导出
 */

#include "history_internal.h"

#include "gui_util.h"
#include "gui_window.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>
#include <time.h>

static void csv_append_double(GString *s, double v)
{
    if (!isfinite(v))
        return;

    char buf[G_ASCII_DTOSTR_BUF_SIZE];

    g_ascii_formatd(buf, sizeof(buf), "%.2f", v);
    g_string_append(s, buf);
}

static void on_history_export_finished(GObject *source, GAsyncResult *res,
                                       gpointer data)
{
    HistoryPage *page = data;
    GError      *err  = NULL;
    GFile       *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source),
                                                    res, &err);

    if (file == NULL) {
        if (err != NULL &&
            !g_error_matches(err, GTK_DIALOG_ERROR,
                             GTK_DIALOG_ERROR_DISMISSED)) {
            GtkRoot *root = gtk_widget_get_root(page->root);

            if (root != NULL && GTK_IS_WINDOW(root))
                fmon_gui_show_error(GTK_WINDOW(root), _("导出失败"),
                                    err->message);
        }
        g_clear_error(&err);
        return;
    }

    char *path = g_file_get_path(file);

    if (path == NULL) {
        g_object_unref(file);
        return;
    }

    GString *csv = g_string_new(
        "Time,Offset_s,CPU_avg,CPU_min,CPU_max,Mem_avg,Mem_min,Mem_max\n");
    gint64 now_mono = g_get_monotonic_time();
    gint64 now_real = g_get_real_time();
    uint64_t latest_t =
        (page->n > 0 && page->n <= HIST_POINTS) ? g_time_us[page->n - 1] : 0;
    guint exported = 0;

    if (page->custom && page->view_end_us != 0)
        latest_t = page->view_end_us;

    for (guint i = 0; i < page->n && i < HIST_POINTS; i++) {
        uint64_t t = g_time_us[i];

        if (page->custom &&
            (t < page->view_start_us || t > page->view_end_us))
            continue;

        exported++;

        if (t != 0) {
            gint64 real = now_real + (gint64)t - now_mono;
            time_t sec  = (time_t)(real / 1000000);

            struct tm tmv;

            if (localtime_r(&sec, &tmv) != NULL) {
                char tb[32];

                strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
                g_string_append(csv, tb);
            }
        }

        char off[32];

        if (t != 0 && latest_t >= t)
            g_ascii_formatd(off, sizeof(off), "%.3f",
                            (double)(latest_t - t) / 1000000.0);
        else
            g_ascii_formatd(off, sizeof(off), "%.3f",
                            (double)i * (double)page->interval_s);
        g_string_append_printf(csv, ",%s,", off);
        csv_append_double(csv, g_cpu[i]);
        g_string_append_c(csv, ',');
        csv_append_double(csv, g_cpu_min[i]);
        g_string_append_c(csv, ',');
        csv_append_double(csv, g_cpu_max[i]);
        g_string_append_c(csv, ',');
        csv_append_double(csv, g_mem[i]);
        g_string_append_c(csv, ',');
        csv_append_double(csv, g_mem_min[i]);
        g_string_append_c(csv, ',');
        csv_append_double(csv, g_mem_max[i]);
        g_string_append_c(csv, '\n');
    }

    GError *werr = NULL;

    if (!g_file_set_contents(path, csv->str, (gssize)csv->len, &werr)) {
        GtkRoot *root = gtk_widget_get_root(page->root);

        if (root != NULL && GTK_IS_WINDOW(root))
            fmon_gui_show_error(GTK_WINDOW(root), _("导出失败"),
                                werr != NULL ? werr->message : path);
        g_clear_error(&werr);
    } else {
        GtkRoot *root = gtk_widget_get_root(page->root);

        if (root != NULL && GTK_IS_WINDOW(root)) {
            char *msg = g_strdup_printf(_("已导出 %u 个采样点到：\n%s"),
                                        exported, path);

            fmon_gui_show_info(GTK_WINDOW(root), _("导出成功"), msg);
            g_free(msg);
        }
    }

    g_string_free(csv, TRUE);
    g_free(path);
    g_object_unref(file);
}

void history_export_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    HistoryPage *page = data;

    if (page->n == 0) {
        GtkRoot *root = gtk_widget_get_root(page->root);

        if (root != NULL && GTK_IS_WINDOW(root))
            fmon_gui_show_error(GTK_WINDOW(root), _("导出失败"),
                                _("当前范围没有历史数据。"));
        return;
    }

    GtkFileDialog *dialog = gtk_file_dialog_new();

    gtk_file_dialog_set_title(dialog, _("保存历史数据 CSV"));
    gtk_file_dialog_set_initial_name(dialog, "fmon_history.csv");
    gtk_file_dialog_set_accept_label(dialog, _("保存"));
    g_object_ref(dialog);
    gtk_file_dialog_save(dialog, GTK_WINDOW(gtk_widget_get_root(page->root)),
                         NULL, on_history_export_finished, page);
    g_object_unref(dialog);
}
