/*
 * process_list.c - 进程页列表构建
 *
 * 模型栈（store → filter → sort → selection）、ColumnView 的列与单元格工厂、
 * 排序器设置。只负责“怎么显示”，不发起命令、不处理信号操作。
 */

#include <glib/gi18n.h>

#include "process_internal.h"

/* 列排序：data 是列号，-1 表示默认排序（用户进程优先 + CPU 降序） */

/* ===========================================================================
 * 单元格工厂
 * ========================================================================= */
static void setup_text_cell(GtkSignalListItemFactory *factory,
                            GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(item, label);
}

static void bind_pid(GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);
    if (p == NULL)
        return;

    gchar buf[16];
    g_snprintf(buf, sizeof(buf), "%u", p->pid);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_category(GtkSignalListItemFactory *f, GtkListItem *item,
                          gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);

    if (p != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           proc_category_name(p->category));
}

static void bind_user(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);
    if (p != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           (p->user != NULL) ? p->user : "");
}

static void bind_state(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);
    if (p == NULL)
        return;

    gchar buf[2] = { p->state, '\0' };
    GtkWidget *lbl = gtk_list_item_get_child(item);

    gtk_label_set_text(GTK_LABEL(lbl), buf);

    const char *desc;

    switch (p->state) {
    case 'R': desc = _("R - 运行中 / 可运行"); break;
    case 'S': desc = _("S - 可中断睡眠"); break;
    case 'I': desc = _("I - 空闲（内核线程）"); break;
    case 'D': desc = _("D - 不可中断睡眠（多在等磁盘 I/O）"); break;
    case 'T': desc = _("T - 已暂停 / 被跟踪"); break;
    case 'Z': desc = _("Z - 僵尸进程"); break;
    case 'W': desc = _("W - 换出等待"); break;
    default:  desc = _("未知状态"); break;
    }

    gtk_widget_set_tooltip_text(lbl, desc);

    if (p->state == 'D')
        gtk_widget_add_css_class(lbl, "health-bad");
    else
        gtk_widget_remove_css_class(lbl, "health-bad");
}

static void bind_time(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);

    if (p == NULL)
        return;

    guint64 s = (guint64)p->cputime_s;
    gchar   buf[32];

    g_snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
               (unsigned long long)(s / 3600),
               (unsigned long long)((s % 3600) / 60),
               (unsigned long long)(s % 60));
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_swap(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);

    if (p == NULL)
        return;

    GtkWidget *lbl = gtk_list_item_get_child(item);

    gtk_widget_set_tooltip_text(
        lbl, _("FreeBSD 不提供每进程当前 swap；"
               "这里显示上次换出前的 RSS（ki_swrss），常为 0"));

    if (p->swap_kb == 0) {
        gtk_label_set_text(GTK_LABEL(lbl), "0");
        return;
    }

    gchar *txt = fmon_gui_format_bytes(p->swap_kb * 1024ULL);

    gtk_label_set_text(GTK_LABEL(lbl), txt);
    g_free(txt);
}

static void bind_ppid(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);

    if (p == NULL)
        return;

    gchar buf[16];

    g_snprintf(buf, sizeof(buf), "%u", p->ppid);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_threads(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);

    if (p == NULL)
        return;

    gchar buf[16];

    g_snprintf(buf, sizeof(buf), "%u", p->threads);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_cpu(GtkSignalListItemFactory *f, GtkListItem *item,
                     gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);
    if (p == NULL)
        return;

    gchar buf[32];
    g_snprintf(buf, sizeof(buf), "%.2f %%", p->cpu);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_mem(GtkSignalListItemFactory *f, GtkListItem *item,
                     gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);
    if (p == NULL)
        return;

    /* 内存显示实际占用（KB/MB/GB 自动单位），比百分比直观 */
    gchar *mem_text = fmon_gui_format_bytes(p->rss_kb * 1024ULL);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), mem_text);
    g_free(mem_text);
}

static void bind_comm(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    ProcItem *p = gtk_list_item_get_item(item);
    if (p == NULL)
        return;

    if (p->depth > 0) {
        GString *s = g_string_new(NULL);

        for (guint16 i = 0; i < p->depth; i++)
            g_string_append(s, "│  ");
        g_string_append(s, "├─ ");
        g_string_append(s, (p->comm != NULL) ? p->comm : "");

        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), s->str);
        g_string_free(s, TRUE);
    } else {
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           (p->comm != NULL) ? p->comm : "");
    }
}

/* ===========================================================================
 * 列表构建
 * ========================================================================= */
GtkWidget *proc_list_build(ProcPage *page)
{
    page->store = g_list_store_new(proc_item_get_type());

    page->filter = gtk_custom_filter_new(proc_filter_func, page, NULL);
    page->filter_model =
        gtk_filter_list_model_new(G_LIST_MODEL(page->store),
                                  GTK_FILTER(page->filter));

    /* 默认排序：用户进程在前，CPU 降序 */
    GtkCustomSorter *default_sorter =
        gtk_custom_sorter_new(proc_compare, GINT_TO_POINTER(-1), NULL);

    page->sort_model =
        gtk_sort_list_model_new(G_LIST_MODEL(page->filter_model),
                                GTK_SORTER(default_sorter));

    page->selection = gtk_multi_selection_new(G_LIST_MODEL(page->sort_model));

    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(page->selection));
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);

    const char *titles[] = { "PID", _("类型"), _("用户"), _("状态"), "CPU",
                             _("内存"), _("CPU 时间"), _("Swap"),
                             "PPID", _("线程"), _("命令") };
    GCallback binders[]  = { (GCallback)bind_pid,      (GCallback)bind_category,
                             (GCallback)bind_user,     (GCallback)bind_state,
                             (GCallback)bind_cpu,      (GCallback)bind_mem,
                             (GCallback)bind_time,     (GCallback)bind_swap,
                             (GCallback)bind_ppid,     (GCallback)bind_threads,
                             (GCallback)bind_comm };

    for (int i = 0; i < PROC_COL_COUNT; i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_signal_connect(factory, "setup", G_CALLBACK(setup_text_cell), NULL);
        g_signal_connect(factory, "bind", binders[i], NULL);

        GtkColumnViewColumn *col =
            gtk_column_view_column_new(titles[i], factory);

        GtkCustomSorter *sorter =
            gtk_custom_sorter_new(proc_compare, GINT_TO_POINTER(i), NULL);
        gtk_column_view_column_set_sorter(col, GTK_SORTER(sorter));

        gtk_column_view_column_set_expand(col, (i == PROC_COL_COMM));
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view), col);
    }
    return view;
}

/* 重新设置一次 filter func 即可让 GtkCustomFilter 重新评估所有行 */
void proc_list_apply_filter(ProcPage *page)
{
    gtk_custom_filter_set_filter_func(page->filter, proc_filter_func, page,
                                      NULL);
}
