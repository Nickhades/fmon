/*
 * service_actions.c - 服务页操作层
 *
 * 启动 / 停止 / 开机自启（只走控制通道，不弹密码框），以及服务详情窗口。
 * 操作完成后通过 svc_page_refresh() 通知页面刷新列表。
 */

#include <glib/gi18n.h>

#include "service_internal.h"

#include "fmon_child.h"
#include "gui_state.h"
#include "gui_util.h"
#include "gui_window.h"
#include "process_view.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 操作上下文（所有权清晰：谁创建谁释放）
 * ========================================================================= */
void svc_actions_op_free(SvcOp *op)
{
    if (op == NULL)
        return;
    g_clear_object(&op->notebook);
    g_free(op->name);
    g_free(op->action);
    g_free(op);
}

static SvcOp *svc_op_clone(const SvcOp *src)
{
    if (src == NULL)
        return NULL;

    SvcOp *op = g_new0(SvcOp, 1);

    op->notebook = (src->notebook != NULL) ? g_object_ref(src->notebook) : NULL;
    op->name     = g_strdup(src->name);
    op->action   = g_strdup(src->action);
    op->enable   = src->enable;
    return op;
}

/* 同步完成：刷新页面并释放（execute 的“消费”语义） */
static void svc_op_complete(SvcOp *op)
{
    if (op->notebook != NULL) {
        ServicePage *page = g_object_get_data(G_OBJECT(op->notebook),
                                              "service-page");

        /* 操作完成后把焦点留在服务页，避免 Enter 落到左侧导航上 */
        if (page != NULL && page->search != NULL)
            gtk_widget_grab_focus(page->search);

        svc_page_refresh(op->notebook);
    }

    svc_actions_op_free(op);
}

static void svc_op_priv_done(gpointer user_data, const FmonChildResult *res,
                             gboolean ok)
{
    SvcOp *op = user_data;

    if (!ok) {
        gchar *msg = g_strdup_printf(_("服务操作“%s”失败"), op->name);

        fmon_gui_show_error(fmon_gui_main_window(), msg,
                            (res != NULL && res->out != NULL)
                                ? res->out
                                : _("命令执行失败"));
        g_free(msg);
    }

    if (op->notebook != NULL)
        svc_page_refresh(op->notebook);

    svc_actions_op_free(op);
}

static void svc_op_run_priv(SvcOp *op, gboolean escalate)
{
    const char *argv[6];
    int         n = 0;
    gchar      *assign = NULL;

    if (escalate)
        argv[n++] = "pkexec";

    if (op->action != NULL) {
        argv[n++] = "/usr/sbin/service";
        argv[n++] = op->name;
        argv[n++] = op->action;
    } else {
        assign = g_strdup_printf("%s=%s", op->name,
                                 op->enable ? "YES" : "NO");
        argv[n++] = "sysrc";
        argv[n++] = assign;
    }

    argv[n] = NULL;

    fmon_gui_run_async_full(argv, 128 * 1024, 30000, svc_op_priv_done, op);
    g_free(assign);
}

static void svc_op_execute(SvcOp *op)
{
    gchar *ctl_err = NULL;

    /* 只走控制通道（守护进程以 root 执行）；失败直接报错 */
    if (fmon_ctl_available()) {
        gchar *err = NULL;

        if (op->action != NULL) {
            FmonCtlResult r = fmon_ctl_service(op->name, op->action, &err);

            if (r == FMON_CTL_OK) {
                g_free(err);
                svc_op_complete(op);
                return;
            }
            ctl_err = g_strdup((err != NULL) ? err : "");
            g_free(err);
        } else {
            FmonCtlResult r = fmon_ctl_sysrc(op->name, op->enable, &err);

            if (r == FMON_CTL_OK) {
                g_free(err);
                svc_op_complete(op);
                return;
            }
            ctl_err = g_strdup((err != NULL) ? err : "");
            g_free(err);
        }
    }

    gboolean not_running = (ctl_err != NULL &&
                            (strstr(ctl_err, "not running") != NULL ||
                             strstr(ctl_err, "未运行") != NULL));

    if (not_running) {
        gchar *msg = g_strdup_printf(_("服务 “%s” 当前未运行"), op->name);

        fmon_gui_show_error(fmon_gui_main_window(), msg,
                            _("请先点“启动”，再执行重启或重载。"));
        g_free(msg);
        g_free(ctl_err);
        svc_op_complete(op);
        return;
    }

    gboolean denied =
        (ctl_err == NULL || ctl_err[0] == '\0' ||
         strcasestr(ctl_err, "permission") != NULL ||
         strcasestr(ctl_err, "denied") != NULL ||
         strstr(ctl_err, "拒绝") != NULL || strstr(ctl_err, "权限") != NULL ||
         strstr(ctl_err, "不允许的动作") != NULL);

    if (denied) {
        g_free(ctl_err);

        if (geteuid() == 0) {
            /* root：控制通道不可用时直接本地执行 */
            svc_op_run_priv(op, FALSE);
            return;
        }

        gchar *pkexec = g_find_program_in_path("pkexec");

        if (pkexec != NULL) {
            g_free(pkexec);
            svc_op_run_priv(op, TRUE);   /* 由 polkit 弹密码授权 */
            return;
        }

        ctl_err = g_strdup(_("控制通道权限不足，且系统没有 pkexec。"));
    }

    gchar *msg = g_strdup_printf(_("无法执行服务操作“%s”。"), op->name);
    GString *detail = g_string_new(NULL);

    if (ctl_err != NULL && ctl_err[0] != '\0')
        g_string_append(detail, ctl_err);
    else
        g_string_append(detail, _("控制通道不可用：请确认 fmon-daemon 正在运行。"));

    if (ctl_err != NULL && strstr(ctl_err, "不允许的动作") != NULL)
        g_string_append(detail,
                        _("\n守护进程版本过旧：请执行 sudo ./install.sh 更新并重启 daemon。"));

    if (geteuid() == 0)
        g_string_append(detail, _("\n当前以 root 运行，请检查服务脚本与 daemon 状态。"));

    fmon_gui_show_error(fmon_gui_main_window(), msg, detail->str);
    g_string_free(detail, TRUE);
    g_free(ctl_err);
    g_free(msg);

    svc_op_complete(op);
}

/* 确认对话框的“确认”回调：op 由 fmon_gui_confirm 释放，这里只能执行副本 */
static void svc_op_confirmed(gpointer user_data)
{
    SvcOp *op = user_data;

    svc_op_execute(svc_op_clone(op));
}

void svc_actions_row_action_free(gpointer data)
{
    RowAction *ra = data;
    if (ra == NULL)
        return;
    g_free(ra->name);
    g_free(ra->action);
    g_free(ra->sysrc);
    g_free(ra);
}

static SvcOp *svc_op_from_row(ServicePage *page, RowAction *ra)
{
    SvcOp *op = g_new0(SvcOp, 1);

    op->notebook = g_object_ref(page->notebook);
    op->name     = g_strdup(ra->name);
    op->action   = (ra->action != NULL) ? g_strdup(ra->action) : NULL;
    op->enable   = (ra->sysrc != NULL && strcmp(ra->sysrc, "YES") == 0);
    return op;
}

void svc_actions_start_stop(GtkButton *btn, gpointer data)
{
    ServicePage *page = data;
    RowAction   *ra   = g_object_get_data(G_OBJECT(btn), "row-action");

    if (ra == NULL || ra->name == NULL)
        return;

    SvcOp *op = svc_op_from_row(page, ra);

    /* 启动/停止都有一定风险，统一二次确认；停止的提示更强烈 */
    if (ra->action != NULL && strcmp(ra->action, "stop") == 0) {
        gchar *msg = g_strdup_printf(_("停止服务 “%s”？"), ra->name);
        fmon_gui_confirm(fmon_gui_main_window(), msg,
                         "该操作会立即终止服务，依赖它的程序可能会出错。",
                         "停止服务", svc_op_confirmed, op,
                         (GDestroyNotify)svc_actions_op_free);
        g_free(msg);
        return;
    }

    svc_op_execute(op);   /* execute 负责释放 op */
}

static void show_service_text(const char *title, const char *text);
static void read_script_deps(const char *name, char **require, char **before);
static gchar *find_script_path(const char *name);

static void add_config_path(GString *out, GHashTable *seen, const char *path)
{
    if (path == NULL || path[0] != '/')
        return;

    if (g_hash_table_contains(seen, path))
        return;

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return;

    g_hash_table_add(seen, g_strdup(path));
    g_string_append(out, path);
    g_string_append_c(out, '\n');
}

static gchar *find_config_paths(const char *name)
{
    static const char *const bases[] = { FMON_LOCALBASE "/etc/",
                                         "/etc/" };

    GString    *out  = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);

    for (size_t i = 0; i < G_N_ELEMENTS(bases); i++) {
        gchar *cands[3];

        cands[0] = g_strdup_printf("%s%s.conf", bases[i], name);
        cands[1] = g_strdup_printf("%s%s/%s.conf", bases[i], name, name);
        cands[2] = g_strdup_printf("%s%s", bases[i], name);

        for (int k = 0; k < 3; k++) {
            add_config_path(out, seen, cands[k]);
            g_free(cands[k]);
        }

        gchar *dir = g_strdup_printf("%s%s", bases[i], name);
        GDir  *d   = g_dir_open(dir, 0, NULL);

        if (d != NULL) {
            const char *entry;

            while ((entry = g_dir_read_name(d)) != NULL) {
                if (!g_str_has_suffix(entry, ".conf") &&
                    !g_str_has_suffix(entry, ".cfg"))
                    continue;

                gchar *p = g_build_filename(dir, entry, NULL);

                add_config_path(out, seen, p);
                g_free(p);
            }

            g_dir_close(d);
        }

        g_free(dir);
    }

    /* 脚本里直接写死的配置路径 */
    gchar *script = find_script_path(name);

    if (script != NULL) {
        gchar *content = NULL;

        if (g_file_get_contents(script, &content, NULL, NULL)) {
            gchar **tok = g_strsplit_set(content, " \t\r\n\"';", -1);

            for (int i = 0; tok[i] != NULL; i++) {
                if (tok[i][0] != '/' ||
                    (strstr(tok[i], ".conf") == NULL &&
                     strstr(tok[i], ".cfg") == NULL))
                    continue;

                add_config_path(out, seen, tok[i]);
            }

            g_strfreev(tok);
            g_free(content);
        }

        g_free(script);
    }

    g_hash_table_destroy(seen);

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return g_strdup(_("未自动找到（可在脚本中查看配置路径）"));
    }

    return g_string_free(out, FALSE);
}

static void dep_tree_build(GString *out, const char *name, int depth,
                           GHashTable *seen)
{
    if (depth > 3 || g_hash_table_contains(seen, name))
        return;

    g_hash_table_add(seen, g_strdup(name));

    for (int i = 0; i < depth; i++)
        g_string_append(out, "    ");
    g_string_append_printf(out, "%s\n", name);

    char *req = NULL, *before = NULL;

    read_script_deps(name, &req, &before);

    /* REQUIRE 与 BEFORE 都展开；seen 去重，避免菱形/循环重复 */
    const char *rels[2] = { req, before };

    for (int r = 0; r < 2; r++) {
        if (rels[r] == NULL)
            continue;

        gchar **deps = g_strsplit_set(rels[r], " \t", -1);

        for (int i = 0; deps[i] != NULL; i++)
            if (deps[i][0] != '\0')
                dep_tree_build(out, deps[i], depth + 1, seen);

        g_strfreev(deps);
    }

    g_free(req);
    g_free(before);
}

static void on_jump_pid(GtkButton *btn, gpointer data)
{
    (void)data;

    guint32 pid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "pid"));

    if (pid == 0)
        return;

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));

    fmon_gui_switch_page("proc");

    gboolean found = gui_process_select_pid(pid);

    if (root != NULL && GTK_IS_WINDOW(root))
        gtk_window_destroy(GTK_WINDOW(root));

    if (!found) {
        gchar *msg = g_strdup_printf(_("进程 %u 不在当前进程视图"), pid);

        fmon_gui_show_error(
            fmon_gui_main_window(), msg,
            _("它可能属于其他用户（切换到系统视图）、是内核线程，或已经退出。"));
        g_free(msg);
    }
}

static void on_show_deps(GtkButton *btn, gpointer data)
{
    (void)data;

    const char *name = g_object_get_data(G_OBJECT(btn), "svc-name");

    if (name == NULL)
        return;

    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);

    g_string_append(out, _("REQUIRE 依赖树（最多 4 层）\n\n"));
    dep_tree_build(out, name, 0, seen);

    gchar *title = g_strdup_printf(_("依赖树 · %s"), name);

    show_service_text(title, out->str);
    g_free(title);
    g_string_free(out, TRUE);
    g_hash_table_destroy(seen);
}

static void show_service_text(const char *title, const char *text)
{
    if (fmon_gui_main_window() == NULL)
        return;

    GtkWidget *win = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 480);
    gtk_window_set_transient_for(GTK_WINDOW(win), fmon_gui_main_window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view   = gtk_text_view_new();
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_buffer_set_text(buf, (text != NULL) ? text : "", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *close = gtk_button_new_with_label(_("关闭"));

    gtk_widget_set_margin_top(close, 8);
    gtk_widget_set_margin_bottom(close, 8);
    gtk_widget_set_margin_end(close, 8);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy),
                             win);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);
    gtk_box_append(GTK_BOX(vbox), close);
    gtk_window_set_child(GTK_WINDOW(win), vbox);
    gtk_window_present(GTK_WINDOW(win));
}

typedef struct {
    char *name;
    char *title;
} SvcTextCtx;

static void svc_text_ctx_free(gpointer data)
{
    SvcTextCtx *ctx = data;

    g_free(ctx->name);
    g_free(ctx->title);
    g_free(ctx);
}

static void on_service_status_done(gpointer user_data,
                                   const FmonChildResult *res, gboolean ok)
{
    SvcTextCtx *ctx = user_data;

    GString *out = g_string_new(NULL);

    g_string_append_printf(out, _("服务：%s\n\n"), ctx->name);

    const gchar *raw = (res != NULL) ? res->out : NULL;
    gboolean not_running = (raw != NULL &&
                            (strstr(raw, "is not running") != NULL ||
                             strstr(raw, "not running") != NULL));

    if (raw != NULL && raw[0] != '\0')
        g_string_append(out, raw);

    if (not_running)
        g_string_append(out, _("\n当前状态：已停止（status 返回非零是正常的）"));
    else if (ok)
        g_string_append(out, _("\n当前状态：运行中"));
    else if (raw != NULL &&
             (strcasestr(raw, "permission") != NULL ||
              strcasestr(raw, "denied") != NULL ||
              strstr(raw, "pidfile") != NULL))
        g_string_append(out, _("\n当前状态：无法读取 pidfile（普通用户权限不足，"
                               "可尝试以 root 运行或检查文件权限）"));
    else
        g_string_append(out, _("\n当前状态：查询失败，请查看上面的原始输出"));

    show_service_text(ctx->title, out->str);
    g_string_free(out, TRUE);
    svc_text_ctx_free(ctx);
}

void svc_actions_status(GtkButton *btn, gpointer data)
{
    (void)data;

    const char *name = g_object_get_data(G_OBJECT(btn), "svc-name");

    if (name == NULL || !fmon_gui_is_valid_token(name))
        return;

    SvcTextCtx *ctx = g_new0(SvcTextCtx, 1);

    ctx->name  = g_strdup(name);
    ctx->title = g_strdup_printf(_("服务状态 · %s"), name);

    const char *argv[] = { "/usr/sbin/service", name, "status", NULL };

    fmon_gui_run_async_full(argv, 128 * 1024, 8000,
                            on_service_status_done, ctx);
}

static void on_service_logs_done(gpointer user_data,
                                 const FmonChildResult *res, gboolean ok)
{
    SvcTextCtx *ctx = user_data;

    GString *out = g_string_new(NULL);

    g_string_append_printf(out, _("服务：%s（最近 500 行 messages）\n\n"),
                           ctx->name);

    if (res != NULL && res->out != NULL && res->out[0] != '\0')
        g_string_append(out, res->out);
    else
        g_string_append(out, _("没有匹配的日志。\n"));

    (void)ok;
    show_service_text(ctx->title, out->str);
    g_string_free(out, TRUE);
    svc_text_ctx_free(ctx);
}

void svc_actions_logs(GtkButton *btn, gpointer data)
{
    (void)data;

    const char *name = g_object_get_data(G_OBJECT(btn), "svc-name");

    if (name == NULL || !fmon_gui_is_valid_token(name))
        return;

    SvcTextCtx *ctx = g_new0(SvcTextCtx, 1);

    ctx->name  = g_strdup(name);
    ctx->title = g_strdup_printf(_("服务日志 · %s"), name);

    /* $1 作为位置参数传入，避免服务名被 shell 解释 */
    const char *argv[] = { "sh", "-c",
                           "tail -n 500 /var/log/messages 2>/dev/null | "
                           "grep -i -F -- \"$1\"",
                           "sh", name, NULL };

    fmon_gui_run_async_full(argv, 256 * 1024, 8000,
                            on_service_logs_done, ctx);
}

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

void svc_actions_restart(GtkButton *btn, gpointer data)
{
    ServicePage *page = data;
    RowAction   *ra   = g_object_get_data(G_OBJECT(btn), "row-action");

    if (ra == NULL || ra->name == NULL)
        return;

    SvcOp *op = svc_op_from_row(page, ra);

    gboolean running = (page->running != NULL) &&
                       g_hash_table_contains(page->running, ra->name);

    if (!running) {
        gboolean enabled = svc_rc_enabled(page->vars, ra->name);

        g_free(op->action);
        op->action = g_strdup(enabled ? "start" : "onestart");

        gchar *msg = g_strdup_printf(
            _("服务 “%s” 未运行，将启动它？"), ra->name);

        fmon_gui_confirm(fmon_gui_main_window(), msg,
                         _("对已停止的服务，重启等价于启动。"),
                         _("启动服务"), svc_op_confirmed, op,
                         (GDestroyNotify)svc_actions_op_free);
        g_free(msg);
        return;
    }

    gchar *msg = g_strdup_printf(_("重启服务 “%s”？"), ra->name);

    fmon_gui_confirm(fmon_gui_main_window(), msg,
                     _("正在使用的连接会短暂中断，服务会停止后重新启动。"),
                     _("重启服务"), svc_op_confirmed, op,
                     (GDestroyNotify)svc_actions_op_free);
    g_free(msg);
}

void svc_actions_reload(GtkButton *btn, gpointer data)
{
    ServicePage *page = data;
    RowAction   *ra   = g_object_get_data(G_OBJECT(btn), "row-action");

    if (ra == NULL || ra->name == NULL)
        return;

    if (!svc_supports_reload(ra->name)) {
        gchar *msg = g_strdup_printf(
            _("服务 “%s” 没有定义 reload 动作"), ra->name);

        fmon_gui_show_error(
            fmon_gui_main_window(), msg,
            _("脚本里没有 reload_cmd / extra_commands；"
              "请先在该 rc.d 脚本中实现 reload，或改用重启。"));
        g_free(msg);
        return;
    }

    SvcOp *op = svc_op_from_row(page, ra);

    if (op->action == NULL)
        op->action = g_strdup("reload");

    svc_op_execute(op);
}

void svc_actions_enable(GtkButton *btn, gpointer data)
{
    ServicePage *page = data;
    RowAction   *ra   = g_object_get_data(G_OBJECT(btn), "row-action");

    if (ra == NULL || ra->sysrc == NULL)
        return;

    SvcOp *op = svc_op_from_row(page, ra);

    if (strcmp(ra->sysrc, "NO") == 0) {
        gchar *msg = g_strdup_printf(_("禁用服务 “%s” 的开机自启？"), ra->name);
        fmon_gui_confirm(fmon_gui_main_window(), msg,
                         "禁用后，系统下次启动时不会再自动运行该服务。",
                         "禁用自启", svc_op_confirmed, op,
                         (GDestroyNotify)svc_actions_op_free);
        g_free(msg);
        return;
    }

    svc_op_execute(op);   /* execute 负责释放 op */
}

/* ===========================================================================
 * 详情窗口
 * ========================================================================= */
static void detail_ctx_free(DetailCtx *ctx)
{
    if (ctx == NULL)
        return;
    g_free(ctx->name);
    g_free(ctx);
}

static gchar *find_script_path(const char *name)
{
    static const char *const dirs[] = { FMON_LOCALBASE "/etc/rc.d",
                                        "/etc/rc.d" };

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        gchar *path = g_build_filename(dirs[d], name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
            return path;

        g_free(path);
    }

    return NULL;
}

/* 读取脚本头部的 REQUIRE / BEFORE 依赖声明 */
static gchar *read_script_rcvar(const char *name)
{
    gchar *path = find_script_path(name);

    if (path == NULL)
        return NULL;

    gchar *content = NULL;
    gchar *out = NULL;

    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);

        for (int i = 0; lines[i] != NULL && i < 120; i++) {
            gchar *l = g_strdup(lines[i]);

            g_strstrip(l);

            if (g_str_has_prefix(l, "rcvar=")) {
                gchar *v = l + strlen("rcvar=");

                g_strstrip(v);

                gsize n = strlen(v);

                if (n >= 2 && (v[0] == '"' || v[0] == '\'') &&
                    v[n - 1] == v[0]) {
                    v[n - 1] = '\0';
                    v++;
                }

                out = g_strdup(v);
                g_free(l);
                break;
            }

            g_free(l);
        }

        g_strfreev(lines);
        g_free(content);
    }

    g_free(path);
    return out;
}

static void read_script_deps(const char *name, char **require, char **before)
{
    *require = NULL;
    *before  = NULL;

    gchar *path = find_script_path(name);

    if (path == NULL)
        return;

    gchar *content = NULL;

    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);

        for (int i = 0; lines[i] != NULL && i < 120; i++) {
            gchar *l = g_strdup(lines[i]);

            g_strstrip(l);

            if (g_str_has_prefix(l, "# REQUIRE:")) {
                g_free(*require);
                *require = g_strdup(l + strlen("# REQUIRE:"));
                g_strstrip(*require);
            } else if (g_str_has_prefix(l, "# BEFORE:")) {
                g_free(*before);
                *before = g_strdup(l + strlen("# BEFORE:"));
                g_strstrip(*before);
            }

            g_free(l);
        }

        g_strfreev(lines);
        g_free(content);
    }

    g_free(path);
}

static gchar *read_script_description(const char *name)
{
    static const char *const dirs[] = { FMON_LOCALBASE "/etc/rc.d",
                                        "/etc/rc.d" };

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        gchar *path = g_build_filename(dirs[d], name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            gchar *content = NULL;

            if (g_file_get_contents(path, &content, NULL, NULL)) {
                GString *desc  = g_string_new(NULL);
                gchar  **lines = g_strsplit(content, "\n", -1);

                for (int i = 0; lines[i] != NULL && i < 60; i++) {
                    gchar *line = g_strdup(lines[i]);
                    g_strstrip(line);
                    if (line[0] == '#') {
                        gchar *text = g_strdup(line + 1);
                        g_strstrip(text);
                        if (text[0] != '\0') {
                            g_string_append(desc, text);
                            g_string_append_c(desc, '\n');
                        }
                        g_free(text);
                    }
                    g_free(line);
                }
                g_strfreev(lines);
                g_free(content);
                g_free(path);

                if (desc->len == 0)
                    g_string_append(desc, "（该脚本没有头部说明）");
                return g_string_free(desc, FALSE);
            }
            g_free(content);
        }
        g_free(path);
    }

    return g_strdup( _("（未找到服务脚本）") );
}

static void add_detail_row(GtkWidget *grid, int *row, const char *key,
                           const char *value)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_widget_add_css_class(k, "dim-label");
    gtk_widget_set_halign(k, GTK_ALIGN_END);
    gtk_widget_set_valign(k, GTK_ALIGN_START);

    GtkWidget *v = gtk_label_new((value != NULL) ? value : "");
    gtk_label_set_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_xalign(GTK_LABEL(v), 0.0f);
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_widget_set_hexpand(v, TRUE);

    gtk_grid_attach(GTK_GRID(grid), k, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, *row, 1, 1);
    (*row)++;
}

static void on_detail_done(gpointer user_data, const FmonChildResult *res,
                           gboolean ok)
{
    DetailCtx *ctx = user_data;

    if (ctx->window == NULL) {   /* 用户已经关掉了窗口 */
        detail_ctx_free(ctx);
        return;
    }

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_margin_top(grid, 18);
    gtk_widget_set_margin_bottom(grid, 18);
    gtk_widget_set_margin_start(grid, 18);
    gtk_widget_set_margin_end(grid, 18);

    int row = 0;
    add_detail_row(grid, &row, "服务名称", ctx->name);

    gchar *script = find_script_path(ctx->name);

    add_detail_row(grid, &row, "脚本路径",
                   (script != NULL) ? script : "（未找到）");
    g_free(script);

    gchar *cfgs = find_config_paths(ctx->name);

    add_detail_row(grid, &row, "配置文件", cfgs);
    g_free(cfgs);

    gchar *rcvar = read_script_rcvar(ctx->name);

    if (rcvar == NULL)
        rcvar = g_strdup_printf("%s_enable", ctx->name);

    add_detail_row(grid, &row, "rc.conf 变量", rcvar);

    GHashTable *vars = svc_rc_load();
    const char *rcval = g_hash_table_lookup(vars, rcvar);

    add_detail_row(grid, &row, "rc.conf 当前值",
                   (rcval != NULL && rcval[0] != '\0') ? rcval : "未设置（默认 NO）");
    g_free(rcvar);

    char *require = NULL, *before = NULL;

    read_script_deps(ctx->name, &require, &before);
    add_detail_row(grid, &row, "依赖 (REQUIRE)",
                   (require != NULL && require[0] != '\0') ? require : "—");
    add_detail_row(grid, &row, "前置 (BEFORE)",
                   (before != NULL && before[0] != '\0') ? before : "—");
    g_free(require);
    g_free(before);

    add_detail_row(grid, &row, "运行状态",
                   ok ? "正在运行 (running)" : "已停止 (stopped)");

    int main_pid = 0;

    if (ok && res != NULL && res->out != NULL) {
        const char *p = strcasestr(res->out, "pid ");

        if (p != NULL)
            main_pid = atoi(p + 4);
    }

    if (main_pid > 0) {
        gchar *ps = g_strdup_printf("%d", main_pid);

        add_detail_row(grid, &row, "主 PID", ps);
        g_free(ps);

        char pidbuf[32];

        g_snprintf(pidbuf, sizeof(pidbuf), "%d", main_pid);

        const char *const argv[] = { "/bin/ps", "-o", "jail=", "-p", pidbuf, NULL };
        FmonChildResult jail_res;

        if (fmon_child_run(argv, 16 * 1024, 1500, &jail_res) == 0 &&
            fmon_child_ok(&jail_res) && jail_res.out != NULL) {
            char *jb = jail_res.out;

            g_strstrip(jb);

            if (jb[0] == '-' || jb[0] == '\0' || strcmp(jb, "0") == 0)
                add_detail_row(grid, &row, "Jail", _("宿主机（非 Jail）"));
            else
                add_detail_row(grid, &row, "Jail", jb);
        }

        fmon_child_result_free(&jail_res);

        const FmonShmData *snap = gui_state_snapshot();

        for (uint32_t i = 0; i < snap->proc_count; i++) {
            const FmonProcInfo *pr = &snap->procs[i];

            if ((int)pr->pid != main_pid)
                continue;

            gchar *cpu = g_strdup_printf("%.2f %%", pr->cpu_usage_pct);
            gchar *mem = fmon_gui_format_bytes(
                (guint64)(pr->mem_rss_kb * 1024.0));

            add_detail_row(grid, &row, "CPU 占用", cpu);
            add_detail_row(grid, &row, "内存 (RSS)", mem);
            g_free(cpu);
            g_free(mem);
            break;
        }

        GtkWidget *jump = gtk_button_new_with_label(
            _("在进程管理中查看"));

        gtk_widget_add_css_class(jump, "suggested-action");
        gtk_widget_set_can_focus(jump, FALSE);
        g_object_set_data(G_OBJECT(jump), "pid", GUINT_TO_POINTER(main_pid));
        g_signal_connect(jump, "clicked", G_CALLBACK(on_jump_pid), NULL);
        gtk_grid_attach(GTK_GRID(grid), jump, 1, row++, 1, 1);
    }

    GtkWidget *depbtn = gtk_button_new_with_label(_("查看依赖树"));

    g_object_set_data_full(G_OBJECT(depbtn), "svc-name",
                           g_strdup(ctx->name), g_free);
    g_signal_connect(depbtn, "clicked", G_CALLBACK(on_show_deps), NULL);
    gtk_grid_attach(GTK_GRID(grid), depbtn, 1, row++, 1, 1);

    gboolean enabled = svc_rc_enabled(vars, ctx->name);
    g_hash_table_destroy(vars);
    add_detail_row(grid, &row, "开机自启", enabled ? "是 (YES)" : "否 (NO)");

    gchar *desc = read_script_description(ctx->name);
    add_detail_row(grid, &row, "服务描述", desc);
    g_free(desc);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *vbox  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *close = gtk_button_new_with_label(_("关闭"));

    gtk_widget_set_margin_top(close, 8);
    gtk_widget_set_margin_bottom(close, 8);
    gtk_widget_set_margin_end(close, 8);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy),
                             ctx->window);
    gtk_box_append(GTK_BOX(vbox), scroll);
    gtk_box_append(GTK_BOX(vbox), close);
    gtk_window_set_child(ctx->window, vbox);
    gtk_window_set_title(ctx->window, _("服务详细信息"));

    detail_ctx_free(ctx);
}

void svc_actions_show_detail(GtkButton *btn, gpointer data)
{
    (void)data;

    const char *name = g_object_get_data(G_OBJECT(btn), "svc-name");

    if (name == NULL || !fmon_gui_is_valid_token(name))
        return;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), _("服务详细信息（加载中…）"));
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 460);
    gtk_window_set_transient_for(GTK_WINDOW(win), fmon_gui_main_window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);

    GtkWidget *loading = gtk_label_new(_("正在查询服务状态…"));
    gtk_window_set_child(GTK_WINDOW(win), loading);
    gtk_window_present(GTK_WINDOW(win));

    DetailCtx *ctx = g_new0(DetailCtx, 1);
    ctx->window = GTK_WINDOW(win);
    ctx->name   = g_strdup(name);

    /* 只持有弱指针：窗口关闭后回调会发现 window == NULL 而安全退出 */
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&ctx->window);

    const char *argv[] = { "service", name, "onestatus", NULL };
    fmon_gui_run_async(argv, on_detail_done, ctx);
}
