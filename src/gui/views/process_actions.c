/*
 * process_actions.c - 进程页操作层
 *
 * 详情窗口、lsof / sockstat 文本窗口、信号与 renice、右键菜单。
 * 只依赖 ProcItem（数据层），不触碰 ProcPage 的列表状态；
 * 所有外部命令一律通过 fmon_gui_run_async* 异步执行。
 */

#include <glib/gi18n.h>

#include "process_internal.h"

#include "cpuset_dialog.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>



/* ===========================================================================
 * 信号 / 优先级操作
 * ========================================================================= */
void proc_actions_signal_op_free(SignalOp *op)
{
    if (op == NULL)
        return;
    g_free(op->name);
    g_free(op);
}

static void show_signal_error(const SignalOp *op, const FmonChildResult *res)
{
    gchar *msg = g_strdup_printf(_("无法向进程 %u (%s) 发送信号。"), op->pid,
                                 (op->name != NULL) ? op->name : "?");
    gchar *detail = NULL;

    if (res != NULL && res->out != NULL && res->out[0] != '\0')
        detail = g_strdup(res->out);
    else
        detail = g_strdup(_("权限不足，或进程已经退出。"));

    fmon_gui_show_error(fmon_gui_main_window(), msg, detail);
    g_free(msg);
    g_free(detail);
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGTERM: return "TERM";
    case SIGKILL: return "KILL";
    case SIGSTOP: return "STOP";
    case SIGCONT: return "CONT";
    case SIGHUP:  return "HUP";
    default:      return NULL;
    }
}

/* 普通用户只能操作自己的进程；root 运行的界面可以操作所有进程。
 * 不再走 pkexec：任何“输密码”的路径都已移除。 */
static gboolean may_operate(const SignalOp *op)
{
    if (op == NULL)
        return FALSE;

    return (op->uid == (guint32)getuid()) || (geteuid() == 0);
}

static void refuse_privileged(const SignalOp *op)
{
    gchar *msg = g_strdup_printf(
        _("无法操作进程 %u：它属于用户 uid %u。"), op->pid, op->uid);

    fmon_gui_show_error(
        fmon_gui_main_window(), msg,
        _("普通用户可以查看其他用户的进程，但不能结束或调整它们。\n"
          "请以 root 运行 fmon-gui（sudo fmon-gui / doas fmon-gui）后再操作。"));
    g_free(msg);
}

void proc_actions_send_signal(gpointer user_data)
{
    /* op 由确认框持有，本函数只借用，不能释放 */
    SignalOp *op = user_data;

    if (op == NULL)
        return;

    if (!may_operate(op)) {
        refuse_privileged(op);
        return;
    }

    if (signal_name(op->sig) == NULL)
        return;

    if (kill((pid_t)op->pid, op->sig) != 0) {
        FmonChildResult fake;

        memset(&fake, 0, sizeof(fake));
        show_signal_error(op, &fake);
    }
}

static void do_renice(gpointer user_data)
{
    /* 同 send_signal：op 由确认框持有，这里不释放 */
    SignalOp *op = user_data;

    if (op == NULL)
        return;

    if (!may_operate(op)) {
        refuse_privileged(op);
        return;
    }

    if (setpriority(PRIO_PROCESS, (id_t)op->pid, op->nice) != 0) {
        gchar *msg = g_strdup_printf(
            _("无法把进程 %u 的优先级调整为 %d。"), op->pid, op->nice);

        fmon_gui_show_error(fmon_gui_main_window(), msg,
                            g_strerror(errno));
        g_free(msg);
    }
}

/* ===========================================================================
 * 结束进程树
 * ========================================================================= */
static GtkWindow *parent_window(GtkWidget *widget);

typedef struct {
    GArray *pids;
    int     sig;
} TreeOp;

static void tree_op_free(gpointer data)
{
    TreeOp *op = data;

    if (op == NULL)
        return;

    g_array_free(op->pids, TRUE);
    g_free(op);
}

static void tree_op_run(gpointer data)
{
    TreeOp *op = data;

    if (op == NULL)
        return;

    /* 子进程先杀，避免父进程退出后子进程被 init 收养 */
    for (gint i = (gint)op->pids->len - 1; i >= 0; i--) {
        guint32 pid = g_array_index(op->pids, guint32, i);

        kill((pid_t)pid, op->sig);
    }
}

static GArray *collect_descendants(guint32 root)
{
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(guint32));

    g_array_append_val(pids, root);

    GHashTable *extra = proc_ps_read_extras();

    if (extra == NULL)
        return pids;

    gboolean added = TRUE;

    while (added) {
        added = FALSE;

        GHashTableIter it;
        gpointer       key, val;

        g_hash_table_iter_init(&it, extra);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            guint32    pid  = *(int *)key;
            ProcExtra *e    = val;

            if (pid == root)
                continue;

            gboolean in = FALSE;

            for (guint i = 0; i < pids->len; i++)
                if (g_array_index(pids, guint32, i) == pid) {
                    in = TRUE;
                    break;
                }

            if (in)
                continue;

            if (e->ppid == root) {
                g_array_append_val(pids, pid);
                added = TRUE;
                continue;
            }

            for (guint i = 0; i < pids->len; i++)
                if (g_array_index(pids, guint32, i) == e->ppid) {
                    g_array_append_val(pids, pid);
                    added = TRUE;
                    break;
                }
        }
    }

    g_hash_table_destroy(extra);
    return pids;
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

static void on_menu_term_tree(GtkButton *btn, gpointer data)
{
    menu_kill_tree(btn, data, SIGTERM);
}

static void on_menu_kill_tree(GtkButton *btn, gpointer data)
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

static gboolean selection_all_owned(ProcPage *page)
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

static void on_menu_batch_term(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcPage   *page    = g_object_get_data(G_OBJECT(popover), "proc-page");

    gtk_popover_popdown(popover);

    if (page != NULL)
        menu_batch(page, parent_window(GTK_WIDGET(btn)), SIGTERM);
}

static void on_menu_batch_kill(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcPage   *page    = g_object_get_data(G_OBJECT(popover), "proc-page");

    gtk_popover_popdown(popover);

    if (page != NULL)
        menu_batch(page, parent_window(GTK_WIDGET(btn)), SIGKILL);
}

static void on_menu_parent(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    ProcPage   *page    = g_object_get_data(G_OBJECT(popover), "proc-page");

    gtk_popover_popdown(popover);

    if (item == NULL || page == NULL || item->ppid == 0)
        return;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(page->selection));

    for (guint i = 0; i < n; i++) {
        ProcItem *p = g_list_model_get_item(G_LIST_MODEL(page->selection), i);

        if (p == NULL)
            continue;

        gboolean match = (p->pid == item->ppid);

        g_object_unref(p);

        if (match) {
            GtkSelectionModel *m = GTK_SELECTION_MODEL(page->selection);

            gtk_selection_model_unselect_all(m);
            gtk_selection_model_select_item(m, i, TRUE);
            return;
        }
    }

    fmon_gui_show_error(
        fmon_gui_main_window(), _("父进程不在当前视图"),
        _("它可能已被过滤（属于系统进程/其他用户），或已经退出。"));
}

static void on_menu_renice_up(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);

    if (item == NULL)
        return;

    SignalOp *op = proc_actions_signal_op_new(item, 0, FALSE);

    op->is_nice   = TRUE;
    op->nice      = 0;
    op->needs_root = (item->uid != getuid());

    gchar *msg = g_strdup_printf(_("把进程 %u（%s）的 nice 提高到 0？"),
                                 item->pid,
                                 (item->comm != NULL) ? item->comm : "?");

    fmon_gui_confirm(parent, msg,
                     _("降低 nice 值需要权限；普通用户只能提高 nice。"),
                     _("提高优先级"), do_renice, op,
                     (GDestroyNotify)proc_actions_signal_op_free);
    g_free(msg);
}

/* ===========================================================================
 * 右键菜单
 * ========================================================================= */
static GtkWindow *parent_window(GtkWidget *widget)
{
    GtkRoot *root = gtk_widget_get_root(widget);
    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static GtkWidget *create_menu_item(const char *text, gboolean is_danger)
{
    GtkWidget *btn = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(btn, "flat");
    if (is_danger)
        gtk_widget_add_css_class(btn, "danger");
    gtk_widget_set_halign(btn, GTK_ALIGN_FILL);

    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(btn));
    if (child != NULL)
        gtk_widget_set_halign(child, GTK_ALIGN_START);

    return btn;
}

static void on_menu_detail(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);
    if (item != NULL)
        proc_windows_show_detail(parent, item);
}

static void on_menu_lsof(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    gchar *pid_str = g_strdup_printf("%u", item->pid);
    const char *argv[] = { "lsof", "-p", pid_str, NULL };

    proc_windows_show_text(parent, "打开的文件 (lsof)", 0, argv);
    g_free(pid_str);
}

static void on_menu_sockstat(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    /* sockstat 没有按 pid 过滤的选项：取全量，在显示层按 PID 过滤 */
    const char *argv[] = { "sockstat", NULL };
    proc_windows_show_text(parent, "网络连接 (sockstat)", item->pid, argv);
}

static void on_menu_copy_pid(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");

    if (item != NULL) {
        gchar *pid_str = g_strdup_printf("%u", item->pid);
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(btn));
        gdk_clipboard_set_text(clipboard, pid_str);
        g_free(pid_str);
    }

    gtk_popover_popdown(popover);
}

SignalOp *proc_actions_signal_op_new(ProcItem *item, int sig,
                                     gboolean needs_confirm)
{
    (void)needs_confirm;

    SignalOp *op = g_new0(SignalOp, 1);

    op->pid        = item->pid;
    op->uid        = item->uid;
    op->name       = g_strdup(item->comm);
    op->sig        = sig;
    op->needs_root = (item->uid != getuid());
    return op;
}

static void on_menu_term(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    SignalOp *op = proc_actions_signal_op_new(item, SIGTERM, TRUE);
    gchar    *msg = g_strdup_printf(_("结束进程 %u（%s）？"), item->pid,
                                    (item->comm != NULL) ? item->comm : "?");

    fmon_gui_confirm(parent, msg,
                     "将先发送 SIGTERM 让进程正常退出。未保存的数据可能丢失。\n若进程属于其他用户，将弹出管理员认证窗口。",
                     "结束进程", proc_actions_send_signal, op, (GDestroyNotify)proc_actions_signal_op_free);
    g_free(msg);
}

static void on_menu_kill(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");
    GtkWindow  *parent  = parent_window(GTK_WIDGET(btn));

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    SignalOp *op  = proc_actions_signal_op_new(item, SIGKILL, TRUE);
    gchar    *msg = g_strdup_printf(_("强制杀死进程 %u（%s）？"), item->pid,
                                    (item->comm != NULL) ? item->comm : "?");

    fmon_gui_confirm(parent, msg,
                     "SIGKILL 无法被捕获，进程会立即终止，未保存的数据会丢失。\n若进程属于其他用户，将弹出管理员认证窗口。",
                     "强制结束", proc_actions_send_signal, op, (GDestroyNotify)proc_actions_signal_op_free);
    g_free(msg);
}

static void on_menu_cpuset(GtkButton *btn, gpointer data)
{
    (void)btn;

    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    fmon_cpuset_dialog_show(parent_window(GTK_WIDGET(popover)), item->pid,
                            item->comm, item->uid);
}

static void on_menu_stop(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    proc_actions_send_signal(proc_actions_signal_op_new(item, SIGSTOP, FALSE));
}

static void on_menu_cont(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    proc_actions_send_signal(proc_actions_signal_op_new(item, SIGCONT, FALSE));
}

static void on_menu_renice(GtkButton *btn, gpointer data)
{
    GtkPopover *popover = GTK_POPOVER(data);
    ProcItem   *item    = g_object_get_data(G_OBJECT(popover), "proc-item");

    gtk_popover_popdown(popover);
    if (item == NULL)
        return;

    SignalOp *op = proc_actions_signal_op_new(item, 0, FALSE);
    op->is_nice   = TRUE;
    op->nice      = 10;
    op->needs_root = (item->uid != getuid());

    do_renice(op);
}

static void on_popover_closed(GtkPopover *popover, gpointer data)
{
    (void)data;
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void on_colview_right_click(GtkGestureClick *gesture, int n_press,
                                   double x, double y, gpointer user_data)
{
    (void)gesture;
    (void)n_press;

    GtkWidget *col_view = GTK_WIDGET(user_data);

    GtkSelectionModel *selection = gtk_column_view_get_model(
        GTK_COLUMN_VIEW(col_view));
    GtkBitset *bits = gtk_selection_model_get_selection(selection);
    guint      pos  = gtk_bitset_is_empty(bits)
                          ? GTK_INVALID_LIST_POSITION
                          : gtk_bitset_get_nth(bits, 0);

    gtk_bitset_unref(bits);

    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    ProcItem *item = g_list_model_get_item(G_LIST_MODEL(selection), pos);

    if (item == NULL)
        return;

    GtkRoot *root = gtk_widget_get_root(col_view);

    if (root == NULL || !GTK_IS_WIDGET(root)) {
        g_object_unref(item);
        return;
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_widget_set_parent(popover, GTK_WIDGET(root));

    /* 菜单持有 item 的引用，关闭时自动释放，不会出现悬垂指针 */
    g_object_set_data_full(G_OBJECT(popover), "proc-item", item,
                           g_object_unref);
    g_object_set_data(G_OBJECT(popover), "proc-page",
                      g_object_get_data(G_OBJECT(col_view), "proc-page"));

    graphene_point_t out;

    if (gtk_widget_compute_point(col_view, GTK_WIDGET(root),
                                 &GRAPHENE_POINT_INIT((float)x, (float)y),
                                 &out)) {
        GdkRectangle rect = { (int)out.x, (int)out.y, 1, 1 };
        gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gboolean allowed = (item->uid == (guint32)getuid() || geteuid() == 0);

    struct {
        const char *label;
        GCallback   cb;
        gboolean    danger;
        gboolean    needs_perm;
        gboolean    stop_item;   /* 暂停/恢复互斥 */
        gboolean    cont_item;
    } entries[] = {
        { "查看详情",        G_CALLBACK(on_menu_detail),   FALSE, FALSE, FALSE, FALSE },
        { "打开的文件",      G_CALLBACK(on_menu_lsof),     FALSE, FALSE, FALSE, FALSE },
        { "网络连接",        G_CALLBACK(on_menu_sockstat), FALSE, FALSE, FALSE, FALSE },
        { "复制 PID",        G_CALLBACK(on_menu_copy_pid), FALSE, FALSE, FALSE, FALSE },
        { "跳转到父进程",    G_CALLBACK(on_menu_parent),   FALSE, FALSE, FALSE, FALSE },
        { "暂停 (SIGSTOP)",  G_CALLBACK(on_menu_stop),     FALSE, TRUE,  TRUE,  FALSE },
        { "恢复 (SIGCONT)",  G_CALLBACK(on_menu_cont),     FALSE, TRUE,  FALSE, TRUE  },
        { "降低优先级 (nice 10)", G_CALLBACK(on_menu_renice), FALSE, TRUE, FALSE, FALSE },
        { "提高优先级 (nice 0)",  G_CALLBACK(on_menu_renice_up), FALSE, TRUE, FALSE, FALSE },
        { "CPU 绑定…",       G_CALLBACK(on_menu_cpuset),   FALSE, TRUE,  FALSE, FALSE },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(entries); i++) {
        GtkWidget *btn = create_menu_item(entries[i].label, entries[i].danger);

        g_signal_connect(btn, "clicked", entries[i].cb, popover);

        if (entries[i].needs_perm && !allowed) {
            gtk_widget_set_sensitive(btn, FALSE);
            gtk_widget_set_tooltip_text(
                btn, _("需要进程属主或 root 权限"));
        }

        if (entries[i].stop_item && item->state == 'T') {
            gtk_widget_set_sensitive(btn, FALSE);
            gtk_widget_set_tooltip_text(btn, _("进程已暂停"));
        }

        if (entries[i].cont_item && item->state != 'T') {
            gtk_widget_set_sensitive(btn, FALSE);
            gtk_widget_set_tooltip_text(btn, _("进程未暂停"));
        }

        gtk_box_append(GTK_BOX(vbox), btn);
    }

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 4);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_box_append(GTK_BOX(vbox), sep);

    GtkWidget *btn_term = create_menu_item("结束进程 (SIGTERM)", FALSE);
    g_signal_connect(btn_term, "clicked", G_CALLBACK(on_menu_term), popover);
    gtk_box_append(GTK_BOX(vbox), btn_term);

    GtkWidget *btn_kill = create_menu_item("强制结束 (SIGKILL)", TRUE);
    g_signal_connect(btn_kill, "clicked", G_CALLBACK(on_menu_kill), popover);
    gtk_box_append(GTK_BOX(vbox), btn_kill);

    GtkWidget *btn_tree = create_menu_item(_("结束进程树 (SIGTERM)"), TRUE);
    g_signal_connect(btn_tree, "clicked", G_CALLBACK(on_menu_term_tree),
                     popover);
    gtk_box_append(GTK_BOX(vbox), btn_tree);

    GtkWidget *btn_tree_kill =
        create_menu_item(_("强制结束进程树 (SIGKILL)"), TRUE);
    g_signal_connect(btn_tree_kill, "clicked",
                     G_CALLBACK(on_menu_kill_tree), popover);
    gtk_box_append(GTK_BOX(vbox), btn_tree_kill);

    if (!allowed) {
        GtkWidget *tb[] = { btn_term, btn_kill, btn_tree, btn_tree_kill };

        for (size_t i = 0; i < G_N_ELEMENTS(tb); i++) {
            gtk_widget_set_sensitive(tb[i], FALSE);
            gtk_widget_set_tooltip_text(tb[i],
                                        _("需要进程属主或 root 权限"));
        }
    }

    ProcPage *page = g_object_get_data(G_OBJECT(col_view), "proc-page");

    if (page != NULL) {
        proc_page_update_hint(page);
        GtkBitset *bs = gtk_selection_model_get_selection(
            GTK_SELECTION_MODEL(page->selection));
        guint64 nsel = gtk_bitset_get_size(bs);

        gtk_bitset_unref(bs);

        if (nsel > 1) {
            gboolean owned = selection_all_owned(page);
            gchar *t = g_strdup_printf(_("结束选中的 %" G_GUINT64_FORMAT " 个进程"),
                                       nsel);
            GtkWidget *b = create_menu_item(t, TRUE);
            g_free(t);
            g_signal_connect(b, "clicked",
                             G_CALLBACK(on_menu_batch_term), popover);

            if (!owned) {
                gtk_widget_set_sensitive(b, FALSE);
                gtk_widget_set_tooltip_text(b, _("选中的进程包含其他用户，需 root"));
            }
            gtk_box_append(GTK_BOX(vbox), b);

            t = g_strdup_printf(_("强制结束选中的 %" G_GUINT64_FORMAT " 个进程"),
                                nsel);
            b = create_menu_item(t, TRUE);
            g_free(t);
            g_signal_connect(b, "clicked",
                             G_CALLBACK(on_menu_batch_kill), popover);

            if (!owned) {
                gtk_widget_set_sensitive(b, FALSE);
                gtk_widget_set_tooltip_text(b, _("选中的进程包含其他用户，需 root"));
            }
            gtk_box_append(GTK_BOX(vbox), b);
        }
    }

    gtk_popover_set_child(GTK_POPOVER(popover), vbox);
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_row_activated(GtkColumnView *view, guint position, gpointer data)
{
    (void)data;

    GtkSelectionModel *model = gtk_column_view_get_model(view);
    GObject *item = g_list_model_get_item(G_LIST_MODEL(model), position);

    if (item == NULL)
        return;

    proc_windows_show_detail(parent_window(GTK_WIDGET(view)), (ProcItem *)item);
    g_object_unref(item);
}

/* 给 ColumnView 挂上右键菜单与“双击查看详情” */
void proc_actions_attach_view(GtkWidget *col_view)
{
    GtkGesture *click = gtk_gesture_click_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 3);
    /* 行内的 cell 会先收到按键；用捕获阶段保证右键菜单一定能弹出 */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_colview_right_click),
                     col_view);
    gtk_widget_add_controller(col_view, GTK_EVENT_CONTROLLER(click));

    g_signal_connect(col_view, "activate", G_CALLBACK(on_row_activated), NULL);
}
