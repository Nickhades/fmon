/*
 * service_actions.c - 服务页操作层
 *
 * 启动 / 停止 / 开机自启（只走控制通道，不弹密码框），以及服务详情窗口。
 * 操作完成后通过 svc_page_refresh() 通知页面刷新列表。
 */

#include <glib/gi18n.h>

#include "service_internal.h"
#include "service_batch.h"

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

gchar *svc_find_config_paths(const char *name)
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
    gchar *script = svc_find_script_path(name);

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

void svc_dep_tree_build(GString *out, const char *name, int depth,
                           GHashTable *seen)
{
    if (depth > 3 || g_hash_table_contains(seen, name))
        return;

    g_hash_table_add(seen, g_strdup(name));

    for (int i = 0; i < depth; i++)
        g_string_append(out, "    ");
    g_string_append_printf(out, "%s\n", name);

    char *req = NULL, *before = NULL;

    svc_read_script_deps(name, &req, &before);

    /* REQUIRE 与 BEFORE 都展开；seen 去重，避免菱形/循环重复 */
    const char *rels[2] = { req, before };

    for (int r = 0; r < 2; r++) {
        if (rels[r] == NULL)
            continue;

        gchar **deps = g_strsplit_set(rels[r], " \t", -1);

        for (int i = 0; deps[i] != NULL; i++)
            if (deps[i][0] != '\0')
                svc_dep_tree_build(out, deps[i], depth + 1, seen);

        g_strfreev(deps);
    }

    g_free(req);
    g_free(before);
}

void svc_actions_status(GtkButton *btn, gpointer data)
{
    svc_detail_status(btn, data);
}

void svc_actions_logs(GtkButton *btn, gpointer data)
{
    svc_detail_logs(btn, data);
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
