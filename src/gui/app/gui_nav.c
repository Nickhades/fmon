/*
 * gui_nav.c - 两级导航（分组 → 子页面）
 *
 * 分组固定在左侧；子页面是内容区顶部的标签条，点击立即切页。
 * 页面切换通过 gui_nav_set_switch() 注册的回调完成，
 * 这样导航模块不依赖 GtkStack 的具体实例。
 */

#include "gui_nav.h"

#include "gui_state.h"
#include "gui_nav_pages.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

typedef struct {
    const char *id;
    const char *title;
    const char *icon;
    const char *desc;
} PageMeta;

typedef struct {
    const char *title;
    const char *icon;
    const char *desc;
    int         pages[12];
    int         n_pages;
} NavGroup;



static GtkWidget *s_sidebar;
static GtkWidget *s_pagebar;
static GtkWidget *s_tabs;
static int        s_cur_group = -1;
static gboolean   s_ready = FALSE;
static gboolean   s_tabs_updating = FALSE;
static gboolean   s_syncing = FALSE;
static GuiNavSwitchFunc s_switch;

static void group_clicked(int gi);
static void on_nav_row_activated(GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data);
static void show_group(int gi);
static gboolean select_tab(const char *page);

static const FmonShmData *snap(void)
{
    return gui_state_snapshot();
}

static void nav_switch(const char *page_id)
{
    if (page_id != NULL && s_switch != NULL)
        s_switch(page_id);
}

static void on_nav_row_selected(GtkListBox *box, GtkListBoxRow *row,
                                gpointer user_data)
{
    (void)box;
    (void)user_data;

    if (row == NULL)
        return;

    int gi = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "group-index"));

    if (gi >= 0)
        group_clicked(gi);
}

/* 启动收尾：先切到目标页，再接管用户点击。
 * 用低优先级 idle，确保 GtkListBox 启动时排队的 row-selected 已经处理完，
 * 不会把 --page 覆盖掉，也不会误切到分组第一页。 */

static void on_nav_row_activated(GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data)
{
    (void)box;
    (void)user_data;

    if (row == NULL)
        return;

    int gi = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "group-index"));

    if (gi >= 0)
        group_clicked(gi);
}

GtkWidget *gui_nav_row_new(const char *title, const char *icon_name,
                                 const char *page_name)
{
    GtkWidget *row  = gtk_list_box_row_new();
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(row, "nav-row");
    gtk_widget_add_css_class(hbox, "nav-item-box");

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *label = gtk_label_new(title);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), label);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    g_object_set_data(G_OBJECT(row), "page-name", (gpointer)page_name);
    g_object_set_data(G_OBJECT(row), "page-title", (gpointer)title);

    return row;
}

static void on_tab_toggled(GtkToggleButton *tab, gpointer data)
{
    (void)data;

    if (s_tabs_updating || !gtk_toggle_button_get_active(tab))
        return;

    const char *page = g_object_get_data(G_OBJECT(tab), "page-name");

    if (page != NULL) {
        select_tab(page);
        nav_switch(page);
    }
}

static GtkWidget *page_tab_new(const char *title, const char *icon_name,
                               const char *page_name)
{
    GtkWidget *tab = gtk_toggle_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_widget_add_css_class(tab, "page-tab");

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);

    GtkWidget *label = gtk_label_new(title);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(tab), box);

    g_object_set_data_full(G_OBJECT(tab), "page-name", g_strdup(page_name),
                           g_free);
    g_signal_connect(tab, "toggled", G_CALLBACK(on_tab_toggled), NULL);

    return tab;
}

/* 只有当前分组真正存在且可见的页面才生成标签 */
static void show_group(int gi)
{
    const GuiNavGroup *grp = gui_nav_group(gi);

    if (grp == NULL || s_tabs == NULL)
        return;

    s_cur_group = gi;
    s_tabs_updating = TRUE;

    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(s_tabs)) != NULL)
        gtk_box_remove(GTK_BOX(s_tabs), child);

    static char gpu_label[FMON_MAX_GPUS][16];
    GtkToggleButton *group = NULL;

    for (int i = 0; i < grp->n_pages; i++) {
        const GuiNavPage *meta = gui_nav_page(grp->pages[i]);
        const char *title = meta->title;

        if (strncmp(meta->id, "gpu", 3) == 0 && meta->id[3] >= '0' &&
            meta->id[3] <= '9') {
            int slot = meta->id[3] - '0';

            if (slot >= (int)snap()->gpu_count)
                continue;

            g_snprintf(gpu_label[slot], sizeof(gpu_label[slot]), "GPU %d",
                       slot + 1);
            title = gpu_label[slot];
        }

        GtkWidget *tab = page_tab_new(title, meta->icon, meta->id);

        if (group == NULL)
            group = GTK_TOGGLE_BUTTON(tab);
        else
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tab), group);

        gtk_box_append(GTK_BOX(s_tabs), tab);
    }

    s_tabs_updating = FALSE;
}

/* 选中顶部标签；page 不属于当前分组时返回 FALSE */
static gboolean select_tab(const char *page)
{
    if (page == NULL || s_tabs == NULL)
        return FALSE;

    gboolean found = FALSE;

    s_tabs_updating = TRUE;

    for (GtkWidget *tab = gtk_widget_get_first_child(s_tabs);
         tab != NULL; tab = gtk_widget_get_next_sibling(tab)) {
        const char *id = g_object_get_data(G_OBJECT(tab), "page-name");
        gboolean same = (id != NULL && strcmp(id, page) == 0);

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tab), same);

        if (same)
            found = TRUE;
    }

    s_tabs_updating = FALSE;
    return found;
}

static const char *first_tab_page(void)
{
    if (s_tabs == NULL)
        return NULL;

    GtkWidget *first = gtk_widget_get_first_child(s_tabs);

    return (first != NULL) ? g_object_get_data(G_OBJECT(first), "page-name")
                           : NULL;
}

/* 点主分组：顶部标签条立即换成该组子页，并切到第一页。
 * 不再滑出第二层侧边栏，内容区因此多出 176px。 */
static void group_clicked(int gi)
{
    if (!s_ready || s_syncing)
        return;

    if (gi != s_cur_group) {
        show_group(gi);

        const char *first = first_tab_page();

        if (first != NULL) {
            select_tab(first);
            nav_switch(first);
        }
    }
}

GtkWidget *gui_nav_sidebar_new(void)
{
    s_sidebar = gtk_list_box_new();
    gtk_widget_add_css_class(s_sidebar, "nav-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s_sidebar),
                                    GTK_SELECTION_SINGLE);
    return s_sidebar;
}

GtkWidget *gui_nav_pagebar_new(void)
{
    s_pagebar = gtk_scrolled_window_new();
    gtk_widget_add_css_class(s_pagebar, "page-tabs");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s_pagebar),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(s_pagebar), FALSE);
    gtk_widget_set_hexpand(s_pagebar, TRUE);
    gtk_widget_set_vexpand(s_pagebar, FALSE);

    s_tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(s_tabs, "page-tab-row");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s_pagebar), s_tabs);

    return s_pagebar;
}

void gui_nav_set_switch(GuiNavSwitchFunc fn)
{
    s_switch = fn;
}

void gui_nav_select_page(const char *page_id)
{
    int idx = gui_nav_find_page(page_id);

    if (idx < 0)
        return;

    int gi = gui_nav_group_of_page(idx);

    if (gi < 0)
        return;

    s_syncing = TRUE;

    if (gi != s_cur_group) {
        show_group(gi);

        GtkListBoxRow *row =
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(s_sidebar), gi);

        if (row != NULL)
            gtk_list_box_select_row(GTK_LIST_BOX(s_sidebar), row);
    }

    if (!select_tab(page_id)) {
        const char *first = first_tab_page();

        if (first != NULL)
            select_tab(first);
    }

    s_syncing = FALSE;
}

void gui_nav_build_groups(void)
{
    for (int i = 0; i < gui_nav_group_count(); i++) {
        const GuiNavGroup *grp = gui_nav_group(i);
        GtkWidget *row = gui_nav_row_new(grp->title, grp->icon, "");

        g_object_set_data(G_OBJECT(row), "group-index", GINT_TO_POINTER(i));
        gtk_list_box_append(GTK_LIST_BOX(s_sidebar), row);
    }
}

static gboolean nav_finish_startup(gpointer data)
{
    char *page = data;

    if (page != NULL)
        nav_switch(page);

    g_signal_connect(s_sidebar, "row-selected",
                     G_CALLBACK(on_nav_row_selected), NULL);
    g_signal_connect(s_sidebar, "row-activated",
                     G_CALLBACK(on_nav_row_activated), NULL);
    s_ready = TRUE;
    g_free(page);
    return G_SOURCE_REMOVE;
}

void gui_nav_start(const char *page_id)
{
    int         start_group = 0;
    const char *start_page = "cpu";

    if (page_id != NULL) {
        int idx = gui_nav_find_page(page_id);

        if (idx >= 0) {
            const GuiNavPage *page = gui_nav_page(idx);

            start_page = page->id;

            int g = gui_nav_group_of_page(idx);

            if (g >= 0)
                start_group = g;
        }
    }

    show_group(start_group);

    if (!select_tab(start_page)) {
        const char *first = first_tab_page();

        if (first != NULL)
            start_page = first;
    }

    gtk_list_box_select_row(
        GTK_LIST_BOX(s_sidebar),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(s_sidebar), start_group));

    g_idle_add_full(G_PRIORITY_LOW, nav_finish_startup, g_strdup(start_page),
                    NULL);
}
