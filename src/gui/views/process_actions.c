/*
 * process_actions.c - 进程页操作层
 *
 * 详情窗口、lsof / sockstat 文本窗口、信号与 renice、右键菜单。
 * 只依赖 ProcItem（数据层），不触碰 ProcPage 的列表状态；
 * 所有外部命令一律通过 fmon_gui_run_async* 异步执行。
 */

#include <glib/gi18n.h>

#include "process_internal.h"
#include "process_menu.h"

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

void refuse_privileged(const SignalOp *op)
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

void do_renice(gpointer user_data)
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



void tree_op_free(gpointer data)
{
    TreeOp *op = data;

    if (op == NULL)
        return;

    g_array_free(op->pids, TRUE);
    g_free(op);
}

void tree_op_run(gpointer data)
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

GArray *collect_descendants(guint32 root)
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

/* 给 ColumnView 挂上右键菜单与“双击查看详情”。 */
void proc_actions_attach_view(GtkWidget *col_view)
{
    proc_menu_attach(col_view);
}
