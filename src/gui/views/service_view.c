/*
 * service_view.c - 服务管理（rc.d）
 *
 * 旧版本的问题：
 *   1. 所有操作都经过 /bin/sh -c "pkexec service %s %s"，服务名来自文件系统。
 *      如果 rc.d 下出现带空格 / $() / ; 的文件名，就会被 shell 解释执行
 *      （命令注入）。现在改为 argv 直传、不经过 shell，并校验服务名字符集。
 *   2. 点击“停止”立即执行，没有二次确认。
 *   3. 启动时同步执行 service -e 与 ls|grep|sort，界面会卡住。
 *   4. 无法搜索，也无法显示“开机自启”状态（要逐个 sysrc 子进程）。
 *   5. 按钮回调把上下文用 g_signal_connect_data 的 GClosureNotify 释放，
 *      类型不匹配（UB），且控件销毁时可能提前释放被异步回调使用的内存。
 *
 * 现在：
 *   - 目录列举用 g_dir_open（无子进程）；
 *   - 开机自启状态直接解析 rc.conf / rc.conf.local / rc.conf.d（无子进程）；
 *   - 运行状态用一次异步 `service -e`；
 *   - 停止 / 禁用需要确认；
 *   - 每个操作使用独立的所有权明确的上下文，不会与控件生命周期纠缠；
 *   - 增加搜索与“隐藏系统内部脚本”开关。
 */

#include <glib/gi18n.h>

#include "service_internal.h"

#include <string.h>

static void service_page_free(gpointer data);






static void on_service_e_done(gpointer user_data, const FmonChildResult *res,
                              gboolean ok)
{
    ServicePage *page = user_data;

    g_hash_table_remove_all(page->running);

    if (ok && res != NULL && res->out != NULL) {
        gchar **lines = g_strsplit(res->out, "\n", -1);

        for (int i = 0; lines[i] != NULL; i++) {
            gchar *line = g_strdup(lines[i]);
            g_strstrip(line);
            if (line[0] == '\0') {
                g_free(line);
                continue;
            }
            const char *base = strrchr(line, '/');
            base = (base != NULL) ? base + 1 : line;
            g_hash_table_add(page->running, g_strdup(base));
            g_free(line);
        }
        g_strfreev(lines);
    }

    page->loading = FALSE;

    /* rc.conf 可能被外部修改过，重新解析 */
    g_hash_table_destroy(page->vars);
    page->vars = svc_rc_load();

    svc_list_rebuild(page);

    /* 释放 svc_page_refresh() 取的引用 */
    if (page->notebook != NULL)
        g_object_unref(page->notebook);
}

void svc_page_refresh(GtkWidget *notebook)
{
    ServicePage *page = g_object_get_data(G_OBJECT(notebook), "service-page");

    if (page == NULL || page->loading)
        return;

    page->loading = TRUE;

    /* 重新扫描目录（可能刚装了新服务） */
    g_ptr_array_free(page->names, TRUE);
    page->names = svc_scan_scripts();

    const char *argv[] = { "service", "-e", NULL };

    g_object_ref(notebook);   /* 回调结束前保证 page 有效 */
    fmon_gui_run_async(argv, on_service_e_done, page);
}

/* ===========================================================================
 * 过滤器
 * ========================================================================= */

static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    (void)entry;
    svc_list_apply_filter(data);
}

static void on_internal_toggled(GtkCheckButton *btn, gpointer data)
{
    ServicePage *page = data;

    page->hide_internal = gtk_check_button_get_active(btn);
    svc_list_apply_filter(page);
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    svc_page_refresh(GTK_WIDGET(data));
}

/* ===========================================================================
 * 页面
 * ========================================================================= */
static void service_page_free(gpointer data)
{
    ServicePage *page = data;

    if (page == NULL)
        return;

    g_clear_pointer(&page->running, g_hash_table_destroy);
    g_clear_pointer(&page->selected, g_hash_table_destroy);
    g_clear_pointer(&page->vars, g_hash_table_destroy);
    if (page->names != NULL)
        g_ptr_array_free(page->names, TRUE);

    g_free(page);
}

GtkWidget *gui_service_view_new(void)
{
    ServicePage *page = g_new0(ServicePage, 1);

    page->running       = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                NULL);
    page->selected      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                NULL);
    page->vars          = svc_rc_load();
    page->names         = svc_scan_scripts();
    page->hide_internal = TRUE;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(vbox, "view-card");
    gtk_widget_set_vexpand(vbox, TRUE);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_refresh = gtk_button_new_with_label(_("刷新"));
    gtk_widget_add_css_class(btn_refresh, "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), btn_refresh);

    GtkWidget *btn_bs = gtk_button_new_with_label(_("启动所选"));
    GtkWidget *btn_bx = gtk_button_new_with_label(_("停止所选"));

    g_signal_connect(btn_bs, "clicked",
                     G_CALLBACK(svc_actions_batch_start), page);
    g_signal_connect(btn_bx, "clicked",
                     G_CALLBACK(svc_actions_batch_stop), page);
    gtk_box_append(GTK_BOX(toolbar), btn_bs);
    gtk_box_append(GTK_BOX(toolbar), btn_bx);

    page->search = gtk_search_entry_new();
    gtk_widget_set_hexpand(page->search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->search), _("搜索服务名"));
    gtk_box_append(GTK_BOX(toolbar), page->search);

    page->btn_internal = gtk_check_button_new_with_label(_("隐藏系统内部脚本"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(page->btn_internal), TRUE);
    gtk_box_append(GTK_BOX(toolbar), page->btn_internal);

    gtk_box_append(GTK_BOX(vbox), toolbar);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    page->notebook = notebook;

    page->list_running = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list_running),
                                    GTK_SELECTION_NONE);
    page->list_stopped = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list_stopped),
                                    GTK_SELECTION_NONE);

    svc_list_setup_filters(page);

    GtkWidget *scroll_stopped = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_stopped),
                                  page->list_stopped);
    page->tab_stopped = gtk_label_new(_("已停止"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll_stopped,
                             page->tab_stopped);

    GtkWidget *scroll_running = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_running),
                                  page->list_running);
    page->tab_running = gtk_label_new(_("正在运行"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll_running,
                             page->tab_running);

    gtk_box_append(GTK_BOX(vbox), notebook);

    /* page 跟随 notebook 生命周期 */
    g_object_set_data_full(G_OBJECT(notebook), "service-page", page,
                           service_page_free);

    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_search_changed), page);
    g_signal_connect(page->btn_internal, "toggled",
                     G_CALLBACK(on_internal_toggled), page);
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     notebook);

    /* 首次加载：先显示“已停止”列表，再异步查询运行状态 */
    svc_list_rebuild(page);
    svc_page_refresh(notebook);

    return vbox;
}
