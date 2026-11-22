/*
 * process_export.c - 进程列表 CSV 导出
 */

#include "process_export.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "csv_util.h"
#include "gui_window.h"
#include "process_internal.h"

#include <errno.h>
#include <stdio.h>

static void on_export_done(GObject *source_object, GAsyncResult *result,
                           gpointer user_data)
{
    GtkFilterListModel *filter_model = user_data;
    GError   *error = NULL;

    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object),
                                              result, &error);

    if (file == NULL) {
        if (error != NULL) {
            if (!g_error_matches(error, GTK_DIALOG_ERROR,
                                 GTK_DIALOG_ERROR_DISMISSED))
                fmon_gui_show_error(fmon_gui_main_window(), _("无法保存文件"),
                                    error->message);
            g_error_free(error);
        }
        g_object_unref(filter_model);
        return;
    }

    gchar *path = g_file_get_path(file);

    if (path == NULL) {
        fmon_gui_show_error(fmon_gui_main_window(), _("导出失败"), _("请选择本地文件系统上的位置。"));
        g_object_unref(file);
        return;
    }

    FILE *fp = fopen(path, "wb");

    if (fp == NULL) {
        gchar *msg = g_strdup_printf(_("无法写入 %s：%s"), path,
                                     g_strerror(errno));
        fmon_gui_show_error(fmon_gui_main_window(), _("导出失败"), msg);
        g_free(msg);
        g_free(path);
        g_object_unref(file);
        g_object_unref(filter_model);
        return;
    }

    fputs("PID,User,System,State,CPU(%),MEM(RSS KB),MEM(%),Command\n", fp);

    guint n = g_list_model_get_n_items(G_LIST_MODEL(filter_model));

    for (guint i = 0; i < n; i++) {
        ProcItem *p = g_list_model_get_item(G_LIST_MODEL(filter_model), i);

        if (p == NULL)
            continue;

        gchar cpu[G_ASCII_DTOSTR_BUF_SIZE];
        gchar mem[G_ASCII_DTOSTR_BUF_SIZE];

        g_ascii_formatd(cpu, sizeof(cpu), "%.2f", p->cpu);
        g_ascii_formatd(mem, sizeof(mem), "%.1f", p->mem);

        char state[2] = { p->state, '\0' };
        char *user = fmon_csv_escape(p->user);
        char *system = fmon_csv_escape(p->is_system ? "system" : "user");
        char *state_s = fmon_csv_escape(state);
        char *cmd = fmon_csv_escape(p->comm);

        fprintf(fp, "%u,%s,%s,%s,%s,%llu,%s,%s\n", p->pid, user, system,
                state_s, cpu, (unsigned long long)p->rss_kb, mem, cmd);

        g_free(user);
        g_free(system);
        g_free(state_s);
        g_free(cmd);
        g_object_unref(p);
    }

    fclose(fp);

    gchar *detail = g_strdup_printf(_("已导出 %u 行到：\n%s"), n, path);
    fmon_gui_show_info(fmon_gui_main_window(), "导出成功", detail);
    g_free(detail);
    g_free(path);
    g_object_unref(file);
    g_object_unref(filter_model);
}

void proc_export_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "保存进程列表 CSV");
    gtk_file_dialog_set_initial_name(dialog, "fmon_processes.csv");
    gtk_file_dialog_set_accept_label(dialog, "保存");

    ProcPage *page = data;

    g_object_ref(dialog);
    gtk_file_dialog_save(dialog, fmon_gui_main_window(), NULL, on_export_done,
                         g_object_ref(page->filter_model));
    g_object_unref(dialog);
}
