/*
 * process_selection.c - 选择保持、滚动恢复与操作可用性
 */

#include "process_selection.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <unistd.h>

#include "process_internal.h"

guint proc_selection_first_pos(ProcPage *page)
{
    GtkBitset *bits = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(page->selection));

    if (gtk_bitset_is_empty(bits)) {
        gtk_bitset_unref(bits);
        return GTK_INVALID_LIST_POSITION;
    }

    guint pos = gtk_bitset_get_nth(bits, 0);

    gtk_bitset_unref(bits);
    return pos;
}

void proc_selection_select(ProcPage *page, guint pos)
{
    GtkSelectionModel *m = GTK_SELECTION_MODEL(page->selection);

    gtk_selection_model_unselect_all(m);

    if (pos != GTK_INVALID_LIST_POSITION)
        gtk_selection_model_select_item(m, pos, TRUE);
}

guint32 proc_selection_selected_pid(ProcPage *page)
{
    guint pos = proc_selection_first_pos(page);

    if (pos == GTK_INVALID_LIST_POSITION)
        return 0;

    ProcItem *item = g_list_model_get_item(G_LIST_MODEL(page->selection), pos);
    guint32   pid  = (item != NULL) ? item->pid : 0;

    if (item != NULL)
        g_object_unref(item);
    return pid;
}

/* 按 pid 恢复选中位置；进程已退出（新数据里没有）时返回 FALSE */
static gboolean restore_selected_pid(ProcPage *page, guint32 pid)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(page->selection));

    for (guint i = 0; i < n; i++) {
        ProcItem *item =
            g_list_model_get_item(G_LIST_MODEL(page->selection), i);
        gboolean hit;

        if (item == NULL)
            continue;
        hit = (item->pid == pid);
        g_object_unref(item);

        if (hit) {
            proc_selection_select(page, i);
            return TRUE;
        }
    }
    return FALSE;
}

/* 重建后恢复选中与滚动位置（在 idle 里做：过滤/排序模型更新完再恢复，
 * 否则可能按旧索引选中错误的行，GTK 还会把视口滚到那一行）。 */
typedef struct {
    ProcPage *page;
    guint32   pid;
    double    vpos;
    int       tries;
} RestoreCtx;

/* 恢复选中行后，GTK 可能把视口滚到选中行（选中项此时也许在视口外）。
 * 列表视图的滚动是延迟到布局阶段才生效的，所以单次 set_value 会被覆盖：
 * 在随后几百毫秒内再钉几次滚动位置，让视口停在用户原来看的地方。 */
static gboolean enforce_scroll_cb(gpointer data)
{
    RestoreCtx    *ctx  = data;
    GtkAdjustment *vadj = NULL;

    if (ctx->page->scroll != NULL)
        vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(ctx->page->scroll));

    if (vadj != NULL)
        gtk_adjustment_set_value(vadj, ctx->vpos);

    if (++ctx->tries >= 3) {
        g_object_unref(ctx->page->root);
        g_free(ctx);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean restore_selection_cb(gpointer data)
{
    RestoreCtx    *ctx  = data;
    GtkAdjustment *vadj = NULL;

    if (ctx->page->scroll != NULL)
        vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(ctx->page->scroll));

    if (!restore_selected_pid(ctx->page, ctx->pid))
        proc_selection_select(ctx->page, GTK_INVALID_LIST_POSITION);

    proc_selection_changed(GTK_SELECTION_MODEL(ctx->page->selection), 0, 0,
                         ctx->page);

    if (vadj != NULL) {
        gtk_adjustment_set_value(vadj, ctx->vpos);
        g_timeout_add(40, enforce_scroll_cb, ctx);
        return G_SOURCE_REMOVE;
    }

    g_object_unref(ctx->page->root);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

/* ps 异步完成：解析 → 指纹比较 → 重建 GListStore */

void proc_selection_schedule_restore(ProcPage *page, guint32 pid, double vpos)
{
    if (page == NULL || page->root == NULL || pid == 0)
        return;

    RestoreCtx *ctx = g_new0(RestoreCtx, 1);

    ctx->page = page;
    ctx->pid = pid;
    ctx->vpos = vpos;
    g_object_ref(page->root);
    g_idle_add(restore_selection_cb, ctx);
}
/* 底部操作栏用的辅助：取当前选中项 */
ProcItem *proc_selection_get_item(ProcPage *page)
{
    guint pos = proc_selection_first_pos(page);

    if (pos == GTK_INVALID_LIST_POSITION)
        return NULL;

    return g_list_model_get_item(G_LIST_MODEL(page->selection), pos);
}

void proc_selection_changed(GtkSelectionModel *sel, guint position,
                                 guint n_items, gpointer data)
{
    (void)sel;
    (void)position;
    (void)n_items;

    ProcPage *page = data;
    gboolean  has  = proc_selection_first_pos(page) !=
                    GTK_INVALID_LIST_POSITION;

    /* 普通用户只能操作自己的进程；root 进程只读。 */
    gboolean can_operate = has;

    if (has && geteuid() != 0) {
        ProcItem *item = proc_selection_get_item(page);

        if (item != NULL) {
            can_operate = (item->uid == (guint32)getuid());
            g_object_unref(item);
        }
    }

    if (page->btn_end != NULL) {
        gtk_widget_set_sensitive(page->btn_end, can_operate);
        gtk_widget_set_tooltip_text(
            page->btn_end,
            can_operate ? _("结束选中的进程（SIGTERM）")
                        : _("普通用户不能操作其他用户的进程；需以 root 运行 fmon-gui"));
    }
    if (page->btn_details != NULL)
        gtk_widget_set_sensitive(page->btn_details, has);

    proc_page_update_hint(page);
}
