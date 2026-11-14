/*
 * cpuset_dialog.c - 进程 CPU 亲和性（cpuset）对话框
 *
 * 读取：`cpuset -g -p <pid>`（无需 root）
 * 修改：优先走控制通道（守护进程以 root 执行 CPUSET），
 *       其次对自己进程直接调用 cpuset，最后回退 pkexec。
 */

#include "cpuset_dialog.h"

#include "gui_ctl.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GtkWidget *win;
    GtkWidget *current;
    GtkWidget *status;
    GtkWidget **checks;
    int        ncpu;
    guint32    pid;
    uid_t      uid;
} CpusetDlg;

static void refresh_current(CpusetDlg *dlg);

static void on_current_done(gpointer user_data, const FmonChildResult *res,
                            gboolean ok)
{
    CpusetDlg *dlg = user_data;

    if (!ok || res == NULL || res->out == NULL) {
        gtk_label_set_text(GTK_LABEL(dlg->current), "（读取失败）");
        return;
    }

    /* 输出形如：pid 123 mask: 0, 1, 2 */
    const char *mask = strstr(res->out, "mask:");

    if (mask == NULL) {
        gtk_label_set_text(GTK_LABEL(dlg->current), "（未找到 mask）");
        return;
    }

    mask += 5;
    while (*mask == ' ')
        mask++;

    gchar *copy = g_strdup(mask);
    gchar *nl   = strchr(copy, '\n');

    if (nl != NULL)
        *nl = '\0';

    gtk_label_set_text(GTK_LABEL(dlg->current), copy);

    for (int i = 0; i < dlg->ncpu; i++) {
        if (dlg->checks[i] != NULL)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(dlg->checks[i]),
                                        FALSE);
    }

    char *save = NULL;

    for (char *tok = strtok_r(copy, ", ", &save); tok != NULL;
         tok = strtok_r(NULL, ", ", &save)) {
        char *dash = strchr(tok, '-');

        if (dash != NULL) {
            int a = atoi(tok);
            int b = atoi(dash + 1);

            if (a > b) { int t = a; a = b; b = t; }
            for (int i = a; i <= b; i++)
                if (i >= 0 && i < dlg->ncpu)
                    gtk_check_button_set_active(
                        GTK_CHECK_BUTTON(dlg->checks[i]), TRUE);
        } else {
            int i = atoi(tok);

            if (i >= 0 && i < dlg->ncpu)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(dlg->checks[i]),
                                            TRUE);
        }
    }

    g_free(copy);
}

static void refresh_current(CpusetDlg *dlg)
{
    gchar *pid_str = g_strdup_printf("%u", dlg->pid);
    const char *argv[] = { "/bin/cpuset", "-g", "-p", pid_str, NULL };

    gtk_label_set_text(GTK_LABEL(dlg->current), "读取中…");
    fmon_gui_run_async_full(argv, 16 * 1024, 3000, on_current_done, dlg);
    g_free(pid_str);
}

static void on_apply_done(gpointer user_data, const FmonChildResult *res,
                          gboolean ok)
{
    CpusetDlg *dlg = user_data;

    if (ok) {
        gtk_label_set_text(GTK_LABEL(dlg->status), "已应用");
        refresh_current(dlg);
    } else {
        gchar *detail = (res != NULL && res->out != NULL && res->out[0] != '\0')
                            ? g_strdup(res->out)
                            : g_strdup("权限不足或 CPU 列表无效");
        gchar *msg = g_strdup_printf("设置失败：%s", detail);

        gtk_label_set_text(GTK_LABEL(dlg->status), msg);
        g_free(msg);
        g_free(detail);
    }
}

static void on_save_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    CpusetDlg *dlg = data;
    GString   *gl  = g_string_new(NULL);

    for (int i = 0; i < dlg->ncpu; i++) {
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(dlg->checks[i])))
            continue;

        if (gl->len > 0)
            g_string_append_c(gl, ',');
        g_string_append_printf(gl, "%d", i);
    }

    if (gl->len == 0) {
        gtk_label_set_text(GTK_LABEL(dlg->status), _("至少选择一个 CPU"));
        g_string_free(gl, TRUE);
        return;
    }

    /* 普通用户只能调整自己的进程；其他用户（含 root）的进程需要 root 运行。 */
    if (dlg->uid != (guint32)getuid() && geteuid() != 0) {
        gtk_label_set_text(GTK_LABEL(dlg->status),
                           "只能调整自己的进程；操作 root 进程需以 root 运行");
        g_string_free(gl, TRUE);
        return;
    }

    /* 1. 优先走控制通道（root 执行） */
    if (fmon_ctl_available()) {
        gchar         *err = NULL;
        FmonCtlResult  r   = fmon_ctl_cpuset((int)dlg->pid, gl->str, &err);

        if (r == FMON_CTL_OK) {
            gtk_label_set_text(GTK_LABEL(dlg->status), "已应用（控制通道）");
            refresh_current(dlg);
            g_free(err);
            g_string_free(gl, TRUE);
            return;
        }
        /* 控制通道不可用或被拒绝：回退到本地/授权执行 */
        g_free(err);
    }

    /* 2. 自己的进程：直接调用，不需要授权 */
    gchar *pid_str = g_strdup_printf("%u", dlg->pid);
    const char *argv[] = { "/bin/cpuset", "-l", gl->str, "-p", pid_str, NULL };

    fmon_gui_run_async_merged(argv, on_apply_done, dlg);
    g_free(pid_str);
    g_string_free(gl, TRUE);
}

static void on_close_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    gtk_window_destroy(GTK_WINDOW(data));
}

static void dlg_free(gpointer data)
{
    CpusetDlg *dlg = data;

    g_free(dlg->checks);
    g_free(dlg);
}

void fmon_cpuset_dialog_show(GtkWindow *parent, guint32 pid,
                             const gchar *comm, uid_t uid)
{
    CpusetDlg *dlg = g_new0(CpusetDlg, 1);

    dlg->pid = pid;
    dlg->uid = uid;

    GtkWidget *win = gtk_window_new();
    dlg->win = win;
    gtk_window_set_title(GTK_WINDOW(win), _("CPU 绑定"));
    gtk_window_set_default_size(GTK_WINDOW(win), 460, -1);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(vbox, 18);
    gtk_widget_set_margin_bottom(vbox, 18);
    gtk_widget_set_margin_start(vbox, 18);
    gtk_widget_set_margin_end(vbox, 18);

    gchar *head = g_strdup_printf("进程 %u（%s）", pid,
                                  (comm != NULL) ? comm : "?");

    GtkWidget *title = gtk_label_new(head);
    gtk_widget_add_css_class(title, "title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), title);
    g_free(head);

    GtkWidget *cur_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *cur_lbl = gtk_label_new("当前绑定：");
    dlg->current = gtk_label_new("读取中…");
    gtk_widget_add_css_class(dlg->current, "small-text");
    gtk_box_append(GTK_BOX(cur_box), cur_lbl);
    gtk_box_append(GTK_BOX(cur_box), dlg->current);
    gtk_box_append(GTK_BOX(vbox), cur_box);

    GtkWidget *entry_lbl = gtk_label_new(_("允许运行的 CPU（勾选）："));
    gtk_widget_set_halign(entry_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), entry_lbl);

    dlg->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (dlg->ncpu <= 0)
        dlg->ncpu = 1;
    if (dlg->ncpu > 256)
        dlg->ncpu = 256;

    dlg->checks = g_new0(GtkWidget *, dlg->ncpu);

    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);

    for (int i = 0; i < dlg->ncpu; i++) {
        gchar *txt = g_strdup_printf("CPU %d", i);

        dlg->checks[i] = gtk_check_button_new_with_label(txt);
        g_free(txt);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(dlg->checks[i]), TRUE);
        gtk_grid_attach(GTK_GRID(grid), dlg->checks[i], i % 8, i / 8, 1, 1);
    }

    gtk_box_append(GTK_BOX(vbox), grid);

    GtkWidget *hint = gtk_label_new(
        "修改其他用户的进程需要 root；如果 fmon-daemon 正在运行，会通过控制通道执行。");
    gtk_widget_add_css_class(hint, "small-text");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), hint);

    dlg->status = gtk_label_new("");
    gtk_widget_add_css_class(dlg->status, "small-text");
    gtk_widget_set_halign(dlg->status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), dlg->status);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);

    GtkWidget *btn_close = gtk_button_new_with_label("关闭");
    GtkWidget *btn_save  = gtk_button_new_with_label("应用");
    gtk_widget_add_css_class(btn_save, "suggested-action");

    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_close_clicked), win);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_clicked), dlg);

    gtk_box_append(GTK_BOX(actions), btn_close);
    gtk_box_append(GTK_BOX(actions), btn_save);
    gtk_box_append(GTK_BOX(vbox), actions);

    gtk_window_set_child(GTK_WINDOW(win), vbox);
    g_object_set_data_full(G_OBJECT(win), "cpuset-dlg", dlg, dlg_free);

    refresh_current(dlg);
    gtk_window_present(GTK_WINDOW(win));
}
