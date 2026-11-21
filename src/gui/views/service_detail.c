/*
 * service_detail.c - 服务详情、依赖树、状态文本窗口
 */

#include "service_internal.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "fmon_child.h"
#include "gui_state.h"
#include "gui_util.h"
#include "gui_window.h"
#include "process_view.h"

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
    svc_dep_tree_build(out, name, 0, seen);

    gchar *title = g_strdup_printf(_("依赖树 · %s"), name);

    svc_show_text_window(title, out->str);
    g_free(title);
    g_string_free(out, TRUE);
    g_hash_table_destroy(seen);
}

void svc_show_text_window(const char *title, const char *text)
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

static void detail_ctx_free(DetailCtx *ctx)
{
    if (ctx == NULL)
        return;
    g_free(ctx->name);
    g_free(ctx);
}

gchar *svc_find_script_path(const char *name)
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
gchar *svc_read_script_rcvar(const char *name)
{
    gchar *path = svc_find_script_path(name);

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

void svc_read_script_deps(const char *name, char **require, char **before)
{
    *require = NULL;
    *before  = NULL;

    gchar *path = svc_find_script_path(name);

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

gchar *svc_read_script_description(const char *name)
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

    gchar *script = svc_find_script_path(ctx->name);

    add_detail_row(grid, &row, "脚本路径",
                   (script != NULL) ? script : "（未找到）");
    g_free(script);

    gchar *cfgs = svc_find_config_paths(ctx->name);

    add_detail_row(grid, &row, "配置文件", cfgs);
    g_free(cfgs);

    gchar *rcvar = svc_read_script_rcvar(ctx->name);

    if (rcvar == NULL)
        rcvar = g_strdup_printf("%s_enable", ctx->name);

    add_detail_row(grid, &row, "rc.conf 变量", rcvar);

    GHashTable *vars = svc_rc_load();
    const char *rcval = g_hash_table_lookup(vars, rcvar);

    add_detail_row(grid, &row, "rc.conf 当前值",
                   (rcval != NULL && rcval[0] != '\0') ? rcval : "未设置（默认 NO）");
    g_free(rcvar);

    char *require = NULL, *before = NULL;

    svc_read_script_deps(ctx->name, &require, &before);
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

    gchar *desc = svc_read_script_description(ctx->name);
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

    svc_show_text_window(ctx->title, out->str);
    g_string_free(out, TRUE);
    svc_text_ctx_free(ctx);
}

void svc_detail_status(GtkButton *btn, gpointer data)
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
    svc_show_text_window(ctx->title, out->str);
    g_string_free(out, TRUE);
    svc_text_ctx_free(ctx);
}

void svc_detail_logs(GtkButton *btn, gpointer data)
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
