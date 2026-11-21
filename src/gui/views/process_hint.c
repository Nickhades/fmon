/*
 * process_hint.c - 进程页提示行（视图说明 + 当前选中 PID）
 */

#include "process_hint.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "process_internal.h"

static void update_hint(ProcPage *page)
{
    gchar *text = NULL;

    if (page->hint == NULL)
        return;

    if (page->view == PROC_VIEW_SYSTEM) {
        if (page->is_root)
            text = g_strdup(_("系统视图：root 与系统账户的进程"));
        else
            text = g_strdup(_("系统进程（只读）：可以查看，但普通用户不能结束/调整；"
                              "操作需以 root 运行 fmon-gui（sudo/doas）"));
    } else if (!page->is_root) {
        text = g_strdup_printf(_("普通用户视图：只显示你自己的进程（uid %u）"),
                               (unsigned)page->uid);
    } else {
        text = g_strdup(_("普通用户视图：所有非系统账户的进程"));
    }

    /* 选中行可能滚出视口（刷新时视口不跟着走），提示里给出 PID，
     * 用户至少知道“结束任务”当前指向哪个进程。 */
    GtkBitset *bits = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(page->selection));
    guint64 nsel = gtk_bitset_get_size(bits);
    guint   pos  = gtk_bitset_is_empty(bits)
                       ? GTK_INVALID_LIST_POSITION
                       : gtk_bitset_get_nth(bits, 0);

    gtk_bitset_unref(bits);

    if (pos != GTK_INVALID_LIST_POSITION) {
        ProcItem *sel =
            g_list_model_get_item(G_LIST_MODEL(page->selection), pos);

        if (sel != NULL) {
            gchar *with_sel;

            if (nsel > 1)
                with_sel = g_strdup_printf(
                    _("%s（已选中 %llu 个进程，自动刷新已暂停）"), text,
                    (unsigned long long)nsel);
            else
                with_sel = g_strdup_printf(
                    _("%s（已选中 PID %u，自动刷新时视口保持不动）"),
                    text, sel->pid);

            g_free(text);
            text = with_sel;
            g_object_unref(sel);
        }
    }

    gtk_label_set_text(GTK_LABEL(page->hint), text);
    gtk_widget_set_visible(page->hint, TRUE);
    g_free(text);
}

void proc_page_update_hint(ProcPage *page)
{
    update_hint(page);
}
