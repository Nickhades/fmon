/*
 * gui_nav.c - 两级导航（分组 → 子页面）
 *
 * 分组列表固定在左侧，二级列表在首次点击分组时滑出。
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
static GtkWidget *s_sub_sidebar;
static GtkWidget *s_sub_list;
static GtkWidget *s_title;
static GtkWidget *s_revealer;
static int        s_cur_group = -1;
static gboolean   s_ready = FALSE;
static GuiNavSwitchFunc s_switch;

static void group_clicked(int gi);
static void on_nav_row_activated(GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data);
static void show_group(int gi);

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
 * 不会把 --page 覆盖掉、也不会误滑出二级列表。 */

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

/* 二级列表：切换分组的子项并默认选中第一项 */
static void show_group(int gi)
{
    const GuiNavGroup *grp = gui_nav_group(gi);

    if (grp == NULL)
        return;

    s_cur_group = gi;

    if (s_title != NULL)
        gtk_label_set_text(GTK_LABEL(s_title), grp->title);

    if (s_sub_list == NULL)
        return;

    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(s_sub_list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(s_sub_list), child);

    static char gpu_label[FMON_MAX_GPUS][16];

    for (int i = 0; i < grp->n_pages; i++) {
        const GuiNavPage *meta = gui_nav_page(grp->pages[i]);

        /* 显卡页：只显示实际存在的 GPU，命名 GPU 1 / GPU 2 ... */
        if (strncmp(meta->id, "gpu", 3) == 0 && meta->id[3] >= '0' &&
            meta->id[3] <= '9') {
            int slot = meta->id[3] - '0';

            if (slot >= (int)snap()->gpu_count)
                continue;

            g_snprintf(gpu_label[slot], sizeof(gpu_label[slot]), "GPU %d",
                       slot + 1);

            GtkWidget *row = gui_nav_row_new(gpu_label[slot], meta->icon,
                                            meta->id);

            gtk_list_box_append(GTK_LIST_BOX(s_sub_list), row);
            continue;
        }

        GtkWidget *row = gui_nav_row_new(meta->title, meta->icon, meta->id);

        gtk_list_box_append(GTK_LIST_BOX(s_sub_list), row);
    }

    GtkListBoxRow *first =
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(s_sub_list), 0);

    if (first != NULL)
        gtk_list_box_select_row(GTK_LIST_BOX(s_sub_list), first);
}

/* 点主分组：首次点击滑出二级列表，并切换到该组第一个子页 */
static void group_clicked(int gi)
{
    if (!s_ready)
        return;

    if (s_revealer != NULL)
        gtk_revealer_set_reveal_child(GTK_REVEALER(s_revealer), TRUE);

    if (gi != s_cur_group) {
        show_group(gi);
    } else if (s_sub_list != NULL) {
        GtkListBoxRow *first =
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(s_sub_list), 0);

        if (first != NULL)
            gtk_list_box_select_row(GTK_LIST_BOX(s_sub_list), first);
    }
}

static void on_sub_row_selected(GtkListBox *box, GtkListBoxRow *row,
                                gpointer user_data)
{
    (void)box;
    (void)user_data;

    if (row == NULL)
        return;

    const char *page = g_object_get_data(G_OBJECT(row), "page-name");

    if (page != NULL)
        nav_switch((const char *)page);
}

GtkWidget *gui_nav_sidebar_new(void)
{
    s_sidebar = gtk_list_box_new();
    gtk_widget_add_css_class(s_sidebar, "nav-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s_sidebar),
                                    GTK_SELECTION_SINGLE);
    return s_sidebar;
}

GtkWidget *gui_nav_revealer_new(void)
{
    s_sub_sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(s_sub_sidebar, "sub-sidebar");
    gtk_widget_set_size_request(s_sub_sidebar, 176, -1);

    s_title = gtk_label_new("");
    gtk_widget_add_css_class(s_title, "sub-title");
    gtk_widget_set_halign(s_title, GTK_ALIGN_START);
    gtk_widget_set_margin_start(s_title, 16);
    gtk_widget_set_margin_top(s_title, 16);
    gtk_widget_set_margin_bottom(s_title, 4);
    gtk_box_append(GTK_BOX(s_sub_sidebar), s_title);

    s_sub_list = gtk_list_box_new();
    gtk_widget_add_css_class(s_sub_list, "nav-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s_sub_list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(s_sub_list, "row-selected",
                     G_CALLBACK(on_sub_row_selected), NULL);

    GtkWidget *sub_scroll = gtk_scrolled_window_new();

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sub_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(sub_scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sub_scroll), s_sub_list);
    gtk_widget_set_vexpand(sub_scroll, TRUE);
    gtk_box_append(GTK_BOX(s_sub_sidebar), sub_scroll);

    s_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(s_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(s_revealer), 260);
    gtk_revealer_set_reveal_child(GTK_REVEALER(s_revealer), FALSE);
    gtk_revealer_set_child(GTK_REVEALER(s_revealer), s_sub_sidebar);

    return s_revealer;
}

void gui_nav_set_switch(GuiNavSwitchFunc fn)
{
    s_switch = fn;
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

    gtk_list_box_select_row(
        GTK_LIST_BOX(s_sidebar),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(s_sidebar), start_group));

    if (page_id != NULL && s_sub_list != NULL) {
        for (int i = 0;; i++) {
            GtkListBoxRow *row =
                gtk_list_box_get_row_at_index(GTK_LIST_BOX(s_sub_list), i);

            if (row == NULL)
                break;

            const char *page = g_object_get_data(G_OBJECT(row), "page-name");

            if (page != NULL && strcmp(page, start_page) == 0) {
                gtk_list_box_select_row(GTK_LIST_BOX(s_sub_list), row);
                break;
            }
        }
    }

    g_idle_add_full(G_PRIORITY_LOW, nav_finish_startup, g_strdup(start_page),
                    NULL);
}
