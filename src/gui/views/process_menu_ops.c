/*
 * process_menu_ops.c - 进程树/批量结束菜单操作
 */

#include "process_menu_ops.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <signal.h>
#include <unistd.h>

#include "gui_util.h"
#include "gui_window.h"
#include "process_internal.h"

static GtkWindow *parent_window(GtkWidget *widget)
{
    GtkRoot *root = gtk_widget_get_root(widget);
    return (root != NULL && GTK_IS_WINDOW(root)) ? GTK_WINDOW(root) : NULL;
}

static void menu_kill_tree(GtkButton *btn, gpointer data, int sig)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);

    if (item == NULL)
        return;

    if (!(item->uid == (guint32)getuid() || geteuid() == 0)) {
        SignalOp *tmp = proc_actions_signal_op_new(item, sig, FALSE);

        refuse_privileged(tmp);
        proc_actions_signal_op_free(tmp);
        return;
    }

    GArray *pids = collect_descendants(item->pid);
    TreeOp *op   = g_new0(TreeOp, 1);

    op->pids = pids;
    op->sig  = sig;

    gchar *msg = g_strdup_printf(
        sig == SIGKILL ? _("强制结束进程树 %u（%s）及其 %u 个子进程？")
                       : _("结束进程树 %u（%s）及其 %u 个子进程？"),
        item->pid, (item->comm != NULL) ? item->comm : "?",
        pids->len > 0 ? pids->len - 1 : 0);

    fmon_gui_confirm(parent, msg,
                     sig == SIGKILL
                         ? _("SIGKILL 无法被捕获，整棵进程树会立即终止。")
                         : _("将向整棵进程树发送 SIGTERM，子进程先退出。"),
                     sig == SIGKILL ? _("强制结束进程树") : _("结束进程树"),
                     tree_op_run, op, tree_op_free);
    g_free(msg);
}

void on_menu_term_tree(GtkButton *btn, gpointer data)
{
    menu_kill_tree(btn, data, SIGTERM);
}

void on_menu_kill_tree(GtkButton *btn, gpointer data)
{
    menu_kill_tree(btn, data, SIGKILL);
}

typedef struct {
    GArray *pids;
    int     sig;
} BatchOp;

static void batch_op_free(gpointer data)
{
    BatchOp *op = data;

    if (op == NULL)
        return;

    g_array_free(op->pids, TRUE);
    g_free(op);
}

static void batch_op_run(gpointer data)
{
    BatchOp *op = data;

    if (op == NULL)
        return;

    for (guint i = 0; i < op->pids->len; i++) {
        guint32 pid = g_array_index(op->pids, guint32, i);

        kill((pid_t)pid, op->sig);
    }
}

static GArray *selected_pids(ProcPage *page)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(guint32));
    GtkBitset *bits = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(page->selection));
    guint64 n = gtk_bitset_get_size(bits);

    for (guint64 i = 0; i < n; i++) {
        guint pos = gtk_bitset_get_nth(bits, i);
        ProcItem *it = g_list_model_get_item(G_LIST_MODEL(page->selection),
                                             pos);

        if (it != NULL) {
            guint32 pid = it->pid;

            g_array_append_val(out, pid);
            g_object_unref(it);
        }
    }

    gtk_bitset_unref(bits);
    return out;
}

gboolean process_menu_selection_all_owned(ProcPage *page)
{
    if (geteuid() == 0)
        return TRUE;

    GtkBitset *bits = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(page->selection));
    guint64 n = gtk_bitset_get_size(bits);
    gboolean ok = TRUE;

    for (guint64 i = 0; i < n; i++) {
        ProcItem *it = g_list_model_get_item(
            G_LIST_MODEL(page->selection), gtk_bitset_get_nth(bits, i));

        if (it != NULL) {
            if (it->uid != (guint32)getuid())
                ok = FALSE;
            g_object_unref(it);
        }

        if (!ok)
            break;
    }

    gtk_bitset_unref(bits);
    return ok;
}

static void menu_batch(ProcPage *page, GtkWindow *parent, int sig)
{
    GArray *pids = selected_pids(page);

    if (pids->len == 0) {
        g_array_free(pids, TRUE);
        return;
    }

    BatchOp *op = g_new0(BatchOp, 1);

    op->pids = pids;
    op->sig  = sig;

    gchar *msg = g_strdup_printf(
        sig == SIGKILL ? _("强制结束选中的 %u 个进程？")
                       : _("结束选中的 %u 个进程？"),
        pids->len);

    fmon_gui_confirm(parent, msg,
                     sig == SIGKILL
                         ? _("SIGKILL 无法被捕获，选中进程会立即终止。")
                         : _("将向选中进程发送 SIGTERM。"),
                     sig == SIGKILL ? _("强制结束") : _("结束进程"),
                     batch_op_run, op, batch_op_free);
    g_free(msg);
}

void on_menu_batch_term(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcPage   *page    = g_object_get_data(G_OBJECT(popover), "proc-page");

    gtk_popover_popdown(popover);

    if (page != NULL)
        menu_batch(page, parent_window(GTK_WIDGET(btn)), SIGTERM);
}

void on_menu_batch_kill(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcPage   *page    = g_object_get_data(G_OBJECT(popover), "proc-page");

    gtk_popover_popdown(popover);

    if (page != NULL)
        menu_batch(page, parent_window(GTK_WIDGET(btn)), SIGKILL);
}
