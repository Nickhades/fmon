/*
 * service_list.c - 服务页列表构建
 *
 * 行构建（状态点、自启标记、四个操作按钮）、列表重建与过滤。
 * 行内按钮只把操作转发给 service_actions.c，这里不执行任何命令。
 */

#include <glib/gi18n.h>

#include "service_internal.h"

#include <string.h>

/* ===========================================================================
 * 行构建
 * ========================================================================= */
static void row_info_free(gpointer data)
{
    RowInfo *ri = data;
    if (ri == NULL)
        return;
    g_free(ri->name);
    g_free(ri);
}

static gboolean row_filter(GtkListBoxRow *row, gpointer user_data)
{
    ServicePage *page = user_data;
    RowInfo     *ri   = g_object_get_data(G_OBJECT(row), "row-info");

    if (ri == NULL)
        return FALSE;
    if (page->hide_internal && ri->internal)
        return FALSE;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(page->search));
    if (text == NULL || text[0] == '\0')
        return TRUE;

    gchar *hay    = g_utf8_strdown(ri->name, -1);
    gchar *needle = g_utf8_strdown(text, -1);
    gboolean keep = (strstr(hay, needle) != NULL);

    g_free(hay);
    g_free(needle);
    return keep;
}

static void on_svc_check_toggled(GtkCheckButton *chk, gpointer data)
{
    ServicePage *page = data;
    const char  *name = g_object_get_data(G_OBJECT(chk), "svc-name");

    if (page == NULL || name == NULL)
        return;

    if (gtk_check_button_get_active(chk))
        g_hash_table_add(page->selected, g_strdup(name));
    else
        g_hash_table_remove(page->selected, name);
}

static GtkWidget *create_button_with_action(ServicePage *page, const char *label,
                                            const char *name, const char *action,
                                            const char *sysrc, gboolean sensitive,
                                            GCallback cb)
{
    GtkWidget *btn = gtk_button_new_with_label(label);

    RowAction *ra = g_new0(RowAction, 1);
    ra->name   = g_strdup(name);
    ra->action = (action != NULL) ? g_strdup(action) : NULL;
    ra->sysrc  = (sysrc != NULL) ? g_strdup(sysrc) : NULL;

    g_object_set_data_full(G_OBJECT(btn), "row-action", ra, svc_actions_row_action_free);
    gtk_widget_set_sensitive(btn, sensitive);
    g_signal_connect(btn, "clicked", cb, page);

    return btn;
}

static GtkWidget *create_service_row(ServicePage *page, const char *name,
                                     gboolean running)
{
    /* 注意：GtkListBox 会自动把子控件包进 GtkListBoxRow，
     * 而 filter_func 收到的是那个包装行。因此这里显式创建 GtkListBoxRow，
     * 并把 row-info 挂在它上面，否则过滤函数永远拿不到数据。 */
    GtkWidget *list_row = gtk_list_box_row_new();
    GtkWidget *row      = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_widget_add_css_class(row, "service-row");

    gboolean enabled = svc_rc_enabled(page->vars, name);

    /* “系统内部脚本”的判定不再依赖一张容易过时的大名单，而是：
     *   不是 /usr/local/etc/rc.d 里的第三方服务
     *   且没有在 rc.conf 中启用
     *   且当前没有运行
     * 这样的脚本对用户基本没有操作价值，默认隐藏。 */
    RowInfo *ri = g_new0(RowInfo, 1);
    ri->name     = g_strdup(name);
    ri->internal = !(svc_is_third_party(name) || enabled || running);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
    g_object_set_data_full(G_OBJECT(list_row), "row-info", ri, row_info_free);

    GtkWidget *chk = gtk_check_button_new();

    g_object_set_data_full(G_OBJECT(chk), "svc-name", g_strdup(name),
                           g_free);
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(chk), g_hash_table_contains(page->selected, name));
    g_signal_connect(chk, "toggled", G_CALLBACK(on_svc_check_toggled), page);
    gtk_box_append(GTK_BOX(row), chk);

    GtkWidget *dot = gtk_label_new("●");
    gtk_widget_add_css_class(
        dot, running ? "status-dot-running" : "status-dot-stopped");
    gtk_box_append(GTK_BOX(row), dot);

    GtkWidget *lbl = gtk_label_new(name);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), lbl);

    GtkWidget *enable_lbl =
        gtk_label_new(enabled ? "开机自启: 是" : "开机自启: 否");
    gtk_widget_add_css_class(enable_lbl, "small-text");
    gtk_widget_set_margin_end(enable_lbl, 10);
    gtk_box_append(GTK_BOX(row), enable_lbl);

    /* “操作”下拉：详情 / 状态 / 日志 / 重启 / 重载，避免一行塞 9 个按钮 */
    GtkWidget *menu = gtk_menu_button_new();

    gtk_menu_button_set_label(GTK_MENU_BUTTON(menu), _("操作"));
    gtk_widget_add_css_class(menu, "flat");

    GtkWidget *pop = gtk_popover_new();
    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *mi_detail = gtk_button_new_with_label(_("详情"));
    gtk_widget_add_css_class(mi_detail, "flat");
    g_object_set_data_full(G_OBJECT(mi_detail), "svc-name", g_strdup(name),
                           g_free);
    g_signal_connect(mi_detail, "clicked",
                     G_CALLBACK(svc_actions_show_detail), NULL);

    GtkWidget *mi_status = gtk_button_new_with_label(_("查询状态"));
    gtk_widget_add_css_class(mi_status, "flat");
    g_object_set_data_full(G_OBJECT(mi_status), "svc-name", g_strdup(name),
                           g_free);
    g_signal_connect(mi_status, "clicked",
                     G_CALLBACK(svc_actions_status), NULL);

    GtkWidget *mi_log = gtk_button_new_with_label(_("查看日志"));
    gtk_widget_add_css_class(mi_log, "flat");
    g_object_set_data_full(G_OBJECT(mi_log), "svc-name", g_strdup(name),
                           g_free);
    g_signal_connect(mi_log, "clicked", G_CALLBACK(svc_actions_logs),
                     NULL);

    GtkWidget *mi_restart =
        create_button_with_action(page, _("重启"), name, "restart", NULL,
                                  TRUE, G_CALLBACK(svc_actions_restart));
    gboolean can_reload = svc_supports_reload(name);
    GtkWidget *mi_reload =
        create_button_with_action(page, _("重载配置"), name, "reload", NULL,
                                  can_reload,
                                  G_CALLBACK(svc_actions_reload));

    if (!can_reload)
        gtk_widget_set_tooltip_text(
            mi_reload, _("该服务脚本没有定义 reload_cmd，无法平滑重载"));

    GtkWidget *items[] = { mi_detail, mi_status, mi_log, mi_restart,
                           mi_reload };

    for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
        gtk_widget_set_halign(items[i], GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(pbox), items[i]);
    }

    gtk_popover_set_child(GTK_POPOVER(pop), pbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), pop);
    gtk_box_append(GTK_BOX(row), menu);

    gtk_box_append(GTK_BOX(row),
                   create_button_with_action(page, "启动", name,
                                             enabled ? "start" : "onestart",
                                             NULL, !running,
                                             G_CALLBACK(svc_actions_start_stop)));
    gtk_box_append(GTK_BOX(row),
                   create_button_with_action(page, "停止", name, "stop", NULL,
                                             running,
                                             G_CALLBACK(svc_actions_start_stop)));
    gtk_box_append(GTK_BOX(row),
                   create_button_with_action(page, "自启", name, NULL, "YES",
                                             !enabled,
                                             G_CALLBACK(svc_actions_enable)));
    gtk_box_append(GTK_BOX(row),
                   create_button_with_action(page, "禁用", name, NULL, "NO",
                                             enabled,
                                             G_CALLBACK(svc_actions_enable)));

    return list_row;
}

/* ===========================================================================
 * 列表重建
 * ========================================================================= */
static void clear_listbox(GtkWidget *list)
{
    GtkWidget *child = NULL;

    while ((child = gtk_widget_get_first_child(list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

static guint count_visible(GtkWidget *list, ServicePage *page)
{
    guint n = 0;

    for (GtkWidget *row = gtk_widget_get_first_child(list);
         row != NULL; row = gtk_widget_get_next_sibling(row)) {
        if (row_filter(GTK_LIST_BOX_ROW(row), page))
            n++;
    }
    return n;
}

void svc_list_rebuild(ServicePage *page)
{
    if (page == NULL || page->names == NULL)
        return;

    /* 清理已经不存在的服务勾选，避免批量操作命中旧名字 */
    if (page->selected != NULL && page->names != NULL) {
        GHashTableIter it;
        gpointer       key;

        g_hash_table_iter_init(&it, page->selected);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            gboolean found = FALSE;

            for (guint i = 0; i < page->names->len; i++)
                if (strcmp(g_ptr_array_index(page->names, i), key) == 0) {
                    found = TRUE;
                    break;
                }

            if (!found)
                g_hash_table_iter_remove(&it);
        }
    }

    clear_listbox(page->list_running);
    clear_listbox(page->list_stopped);

    guint n_running = 0, n_stopped = 0;

    for (guint i = 0; i < page->names->len; i++) {
        const char *name    = g_ptr_array_index(page->names, i);
        gboolean    running = g_hash_table_contains(page->running, name);

        GtkWidget *row = create_service_row(page, name, running);

        if (running) {
            gtk_list_box_append(GTK_LIST_BOX(page->list_running), row);
            n_running++;
        } else {
            gtk_list_box_append(GTK_LIST_BOX(page->list_stopped), row);
            n_stopped++;
        }
    }

    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page->list_running));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page->list_stopped));

    gchar *lbl = g_strdup_printf(_("正在运行 (%u 可见 / 共 %u)"),
                                 count_visible(page->list_running, page),
                                 n_running);
    gtk_label_set_text(GTK_LABEL(page->tab_running), lbl);
    g_free(lbl);

    lbl = g_strdup_printf(_("已停止 (%u 可见 / 共 %u)"),
                          count_visible(page->list_stopped, page), n_stopped);
    gtk_label_set_text(GTK_LABEL(page->tab_stopped), lbl);
    g_free(lbl);
}

/* 给两个列表挂上过滤函数（页面装配时调用一次） */
void svc_list_setup_filters(ServicePage *page)
{
    gtk_list_box_set_filter_func(GTK_LIST_BOX(page->list_running), row_filter,
                                 page, NULL);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(page->list_stopped), row_filter,
                                 page, NULL);
}

void svc_list_apply_filter(ServicePage *page)
{
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page->list_running));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page->list_stopped));

    /* 计数也要跟着过滤条件变化 */
    guint n_running = 0, n_stopped = 0;

    for (GtkWidget *row = gtk_widget_get_first_child(page->list_running);
         row != NULL; row = gtk_widget_get_next_sibling(row))
        n_running++;
    for (GtkWidget *row = gtk_widget_get_first_child(page->list_stopped);
         row != NULL; row = gtk_widget_get_next_sibling(row))
        n_stopped++;

    gchar *lbl = g_strdup_printf(_("正在运行 (%u 可见 / 共 %u)"),
                                 count_visible(page->list_running, page),
                                 n_running);
    gtk_label_set_text(GTK_LABEL(page->tab_running), lbl);
    g_free(lbl);

    lbl = g_strdup_printf(_("已停止 (%u 可见 / 共 %u)"),
                          count_visible(page->list_stopped, page), n_stopped);
    gtk_label_set_text(GTK_LABEL(page->tab_stopped), lbl);
    g_free(lbl);
}
