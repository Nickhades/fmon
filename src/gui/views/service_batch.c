/*
 * service_batch.c - 服务批量启停
 */

#include "service_batch.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "gui_ctl.h"
#include "gui_window.h"
#include "service_internal.h"
#include "service_rc.h"

typedef struct {
    GPtrArray *names;
    GPtrArray *actions;   /* 与 names 平行：start/onestart/stop */
    GtkWidget *notebook;
    GtkWidget *dialog;
    GtkWidget *bar;
    guint      timer;
    gint       done;
    gint       failed;
    gint       cancelled;
    gchar     *last_err;
    gboolean   started;
} BatchSvc;

static void batch_svc_free(gpointer data)
{
    BatchSvc *b = data;

    if (b == NULL)
        return;

    g_ptr_array_free(b->names, TRUE);
    if (b->actions != NULL)
        g_ptr_array_free(b->actions, TRUE);
    g_free(b->last_err);
    if (b->notebook != NULL)
        g_object_unref(b->notebook);
    g_free(b);
}

static gboolean batch_tick(gpointer data)
{
    BatchSvc *b = data;
    int total = (int)b->names->len;

    if (b->bar != NULL && total > 0)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->bar),
                                      (double)g_atomic_int_get(&b->done) /
                                          (double)total);
    return G_SOURCE_CONTINUE;
}

static void batch_cancel_cb(GtkButton *btn, gpointer data)
{
    (void)btn;

    BatchSvc *b = data;

    g_atomic_int_set(&b->cancelled, 1);
}

static void batch_thread(GTask *task, gpointer src, gpointer data,
                         GCancellable *cancellable)
{
    (void)task;
    (void)src;
    (void)cancellable;

    BatchSvc *b = data;

    for (guint i = 0; i < b->names->len; i++) {
        if (g_atomic_int_get(&b->cancelled))
            break;

        gchar *err = NULL;

        const char *act = g_ptr_array_index(b->actions, i);

        FmonCtlResult r = fmon_ctl_service(g_ptr_array_index(b->names, i),
                                          act, &err);

        if (r != FMON_CTL_OK) {
            g_atomic_int_inc(&b->failed);
            g_free(b->last_err);
            b->last_err = g_strdup((err != NULL && err[0] != '\0')
                                       ? err
                                       : _("控制通道不可用"));
        }

        g_free(err);
        g_atomic_int_inc(&b->done);
    }
}

static void batch_dialog_destroyed(GtkWidget *win, gpointer data)
{
    (void)win;

    BatchSvc *b = data;

    if (b->timer != 0) {
        g_source_remove(b->timer);
        b->timer = 0;
    }

    b->bar = NULL;
    b->dialog = NULL;
}

static void batch_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;

    GTask    *task = G_TASK(res);
    BatchSvc *b    = g_task_get_task_data(task);

    if (b->timer != 0)
        g_source_remove(b->timer);

    if (b->dialog != NULL)
        gtk_window_destroy(GTK_WINDOW(b->dialog));

    if (b->notebook != NULL) {
        ServicePage *sp = g_object_get_data(G_OBJECT(b->notebook),
                                            "service-page");

        if (sp != NULL)
            g_hash_table_remove_all(sp->selected);

        svc_page_refresh(b->notebook);
    }

    if (b->failed > 0) {
        gchar *msg = g_strdup_printf(_("%d 个服务操作失败"), b->failed);

        fmon_gui_show_error(fmon_gui_main_window(), msg, b->last_err);
        g_free(msg);
    }

    if (g_atomic_int_get(&b->cancelled)) {
        gchar *msg = g_strdup_printf(
            _("批量操作已取消：完成 %d / %u"),
            g_atomic_int_get(&b->done), b->names->len);

        fmon_gui_show_error(fmon_gui_main_window(), _("已取消"), msg);
        g_free(msg);
    }

    /* task 的生命周期由 g_task_run_in_thread()/回调框架管理，
     * 这里不能 unref，否则回调返回时会二次释放。 */
}

static void batch_svc_run(gpointer data)
{
    BatchSvc *b = data;

    b->started = TRUE;

    GtkWidget *win = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(win), _("批量服务操作"));
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 120);
    gtk_window_set_transient_for(GTK_WINDOW(win), fmon_gui_main_window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_deletable(GTK_WINDOW(win), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    gchar *lbl = g_strdup_printf(_("正在处理 %u 个服务…"),
                                 b->names->len);
    GtkWidget *l = gtk_label_new(lbl);

    g_free(lbl);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), l);

    GtkWidget *bar = gtk_progress_bar_new();

    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
    gtk_box_append(GTK_BOX(box), bar);

    GtkWidget *cancel = gtk_button_new_with_label(_("取消"));

    gtk_widget_set_halign(cancel, GTK_ALIGN_END);
    g_signal_connect(cancel, "clicked", G_CALLBACK(batch_cancel_cb), b);
    gtk_box_append(GTK_BOX(box), cancel);

    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));

    b->dialog = win;
    b->bar = bar;
    g_signal_connect(win, "destroy", G_CALLBACK(batch_dialog_destroyed), b);
    b->timer = g_timeout_add(120, batch_tick, b);

    GTask *task = g_task_new(NULL, NULL, batch_done, NULL);

    g_task_set_task_data(task, b, batch_svc_free);
    g_task_run_in_thread(task, batch_thread);
    g_object_unref(task);
}

static void batch_svc_destroy(gpointer data)
{
    BatchSvc *b = data;

    /* 用户取消确认框时确认回调没跑，b 还没交给 GTask */
    if (!b->started)
        batch_svc_free(b);
}

static void batch_svc_confirm(ServicePage *page, const char *action)
{
    if (g_hash_table_size(page->selected) == 0) {
        fmon_gui_show_error(fmon_gui_main_window(), _("没有选中服务"),
                            _("勾选左侧服务后再执行批量操作。"));
        return;
    }

    BatchSvc *b = g_new0(BatchSvc, 1);

    b->names = g_ptr_array_new_with_free_func(g_free);
    b->actions = g_ptr_array_new_with_free_func(g_free);
    b->notebook = g_object_ref(page->notebook);

    GList *keys = g_hash_table_get_keys(page->selected);

    for (GList *it = keys; it != NULL; it = it->next) {
        const char *name = it->data;

        g_ptr_array_add(b->names, g_strdup(name));

        if (strcmp(action, "start") == 0) {
            gboolean enabled = svc_rc_enabled(page->vars, name);

            g_ptr_array_add(b->actions,
                            g_strdup(enabled ? "start" : "onestart"));
        } else {
            g_ptr_array_add(b->actions, g_strdup(action));
        }
    }
    g_list_free(keys);

    gchar *msg = g_strdup_printf(
        _("对选中的 %u 个服务执行 %s？"), b->names->len,
        strcmp(action, "start") == 0 ? _("启动") : _("停止"));

    fmon_gui_confirm(fmon_gui_main_window(), msg,
                     _("操作会通过 root 守护进程执行。"),
                     _("执行"), batch_svc_run, b, batch_svc_destroy);
    g_free(msg);
}

void svc_actions_batch_start(GtkButton *btn, gpointer data)
{
    (void)btn;
    batch_svc_confirm(data, "start");
}

void svc_actions_batch_stop(GtkButton *btn, gpointer data)
{
    (void)btn;
    batch_svc_confirm(data, "stop");
}
