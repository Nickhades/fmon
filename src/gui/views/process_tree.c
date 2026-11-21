/*
 * process_tree.c - 进程树排序与树形模式切换
 */

#include "process_tree.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "process_internal.h"

/* 树形视图：按 ppid 深度优先排序，并在命令列缩进 */
static gint cmp_item_pid(gconstpointer a, gconstpointer b)
{
    const ProcItem *x = *(ProcItem *const *)a;
    const ProcItem *y = *(ProcItem *const *)b;

    return (x->pid > y->pid) - (x->pid < y->pid);
}

typedef struct {
    GHashTable *kids;
    GHashTable *seen;
    GArray     *flat;
} TreeCtx;

static void tree_add(ProcItem *it, guint16 depth, TreeCtx *ctx)
{
    if (g_hash_table_contains(ctx->seen, GUINT_TO_POINTER(it->pid)))
        return;

    g_hash_table_add(ctx->seen, GUINT_TO_POINTER(it->pid));
    it->depth = depth;
    g_array_append_val(ctx->flat, it);

    GPtrArray *list = g_hash_table_lookup(ctx->kids,
                                          GUINT_TO_POINTER(it->pid));

    if (list == NULL)
        return;

    g_ptr_array_sort(list, cmp_item_pid);

    for (guint i = 0; i < list->len; i++)
        tree_add(g_ptr_array_index(list, i), depth + 1, ctx);
}

void proc_tree_sort_items(ProcItem **items, guint n, ProcPage *page)
{
    if (n == 0)
        return;

    GHashTable *by_pid = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *kids   = g_hash_table_new(g_direct_hash, g_direct_equal);
    GPtrArray  *roots  = g_ptr_array_new();
    GPtrArray  *vis    = g_ptr_array_new();

    for (guint i = 0; i < n; i++) {
        ProcItem *it = items[i];

        it->depth = 0;

        /* 只在“当前视图可见”的进程之间建父子关系；
         * 否则被过滤掉的 PID 1 会让所有可见行都变成缩进子节点。 */
        if (proc_filter_func(it, page))
            g_ptr_array_add(vis, it);
    }

    for (guint i = 0; i < vis->len; i++) {
        ProcItem *it = g_ptr_array_index(vis, i);

        g_hash_table_insert(by_pid, GUINT_TO_POINTER(it->pid), it);
    }

    for (guint i = 0; i < vis->len; i++) {
        ProcItem *it = g_ptr_array_index(vis, i);

        if (it->ppid != 0 && it->ppid != it->pid &&
            g_hash_table_contains(by_pid, GUINT_TO_POINTER(it->ppid))) {
            GPtrArray *list = g_hash_table_lookup(kids,
                                                  GUINT_TO_POINTER(it->ppid));

            if (list == NULL) {
                list = g_ptr_array_new();
                g_hash_table_insert(kids, GUINT_TO_POINTER(it->ppid), list);
            }
            g_ptr_array_add(list, it);
        } else {
            g_ptr_array_add(roots, it);
        }
    }

    g_ptr_array_sort(roots, cmp_item_pid);

    TreeCtx ctx = {
        .kids = kids,
        .seen = g_hash_table_new(g_direct_hash, g_direct_equal),
        .flat = g_array_new(FALSE, FALSE, sizeof(ProcItem *)),
    };

    for (guint i = 0; i < roots->len; i++)
        tree_add(g_ptr_array_index(roots, i), 0, &ctx);

    /* 环/异常 ppid：剩余可见节点按 pid 追加，避免丢失 */
    for (guint i = 0; i < vis->len; i++)
        if (!g_hash_table_contains(
                ctx.seen,
                GUINT_TO_POINTER(((ProcItem *)g_ptr_array_index(vis, i))->pid)))
            tree_add(g_ptr_array_index(vis, i), 0, &ctx);

    /* 被过滤的进程追加到末尾（过滤器会隐藏它们） */
    for (guint i = 0; i < n; i++)
        if (!proc_filter_func(items[i], page)) {
            items[i]->depth = 0;
            g_array_append_val(ctx.flat, items[i]);
        }

    /* 正常路径每个 item 恰好进入 flat 一次。若 ps 出现同一 PID 的
     * 重复行或过滤状态变化导致数量不一致，保持原顺序，绝不越界读。 */
    if (ctx.flat->len == n) {
        for (guint i = 0; i < n; i++)
            items[i] = g_array_index(ctx.flat, ProcItem *, i);
    }

    g_ptr_array_free(vis, TRUE);
    g_array_free(ctx.flat, TRUE);
    g_hash_table_destroy(ctx.seen);
    g_ptr_array_free(roots, TRUE);

    GHashTableIter hi;
    gpointer       key, val;

    g_hash_table_iter_init(&hi, kids);
    while (g_hash_table_iter_next(&hi, &key, &val))
        g_ptr_array_free(val, TRUE);

    g_hash_table_destroy(kids);
    g_hash_table_destroy(by_pid);
}

void proc_tree_sorter_changed(GtkSorter *sorter, GParamSpec *pspec,
                                  gpointer data)
{
    (void)pspec;

    ProcPage *page = data;

    if (page->tree_mode)
        return;

    gtk_sort_list_model_set_sorter(page->sort_model, sorter);
}

void proc_tree_toggled(GtkToggleButton *btn, gpointer data)
{
    ProcPage *page = data;

    page->tree_mode = gtk_toggle_button_get_active(btn);

    if (page->tree_mode) {
        gtk_sort_list_model_set_sorter(page->sort_model, NULL);
        proc_view_scroll_top(page);

        guint     n   = g_list_model_get_n_items(G_LIST_MODEL(page->store));
        ProcItem **arr = g_new0(ProcItem *, n);

        for (guint i = 0; i < n; i++)
            arr[i] = g_list_model_get_item(G_LIST_MODEL(page->store), i);

        proc_tree_sort_items(arr, n, page);

        /* 不在已经被 ColumnView/SelectionModel 引用的 store 上
         * remove/splice 中间态，而是构建一个新 store 后整体替换。
         * 这样模型只经历一次 reset，不会在空模型和旧索引之间
         * 出现 GTK 内部重排。 */
        GListStore *reordered = g_list_store_new(proc_item_get_type());

        g_list_store_splice(reordered, 0, 0, (gpointer *)arr, n);

        /* GtkFilterListModel 接管 model 的引用；page->store 是借用指针。
         * 这里不能再 unref reordered，也不能 g_set_object 旧 store，
         * 否则会对已经被 filter model 释放的旧 store 二次 unref。 */
        gtk_filter_list_model_set_model(page->filter_model,
                                        G_LIST_MODEL(reordered));
        page->store = reordered;

        for (guint i = 0; i < n; i++)
            g_object_unref(arr[i]);
        g_free(arr);
    } else {
        GtkCustomSorter *s = gtk_custom_sorter_new(proc_compare,
                                                   GINT_TO_POINTER(-1), NULL);

        gtk_sort_list_model_set_sorter(page->sort_model, GTK_SORTER(s));
    }
}
