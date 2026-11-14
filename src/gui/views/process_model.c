/*
 * process_model.c - 进程页数据层
 *
 * 只做数据：ProcItem GObject、视图过滤、列排序。
 * ps 输出解析在 process_ps.c（纯 GLib，可单测）；
 * 不创建 GtkWidget，也不发起子进程（ps 的调用留在 process_view.c）。
 */

#include "process_internal.h"

#include <string.h>

/* ===========================================================================
 * ProcItem
 * ========================================================================= */
G_DEFINE_TYPE(ProcItem, proc_item, G_TYPE_OBJECT)

static void proc_item_finalize(GObject *obj)
{
    ProcItem *self = (ProcItem *)obj;

    g_free(self->user);
    g_free(self->comm);

    G_OBJECT_CLASS(proc_item_parent_class)->finalize(obj);
}

static void proc_item_class_init(ProcItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = proc_item_finalize;
}

static void proc_item_init(ProcItem *self)
{
    (void)self;
}


/* ===========================================================================
 * 过滤 / 排序
 * ========================================================================= */
gboolean proc_filter_func(gpointer item, gpointer user_data)
{
    ProcItem *p    = item;
    ProcPage *page = user_data;

    if (page == NULL)
        return TRUE;

    /* 视图过滤：系统进程只属于“系统”视图；普通用户视图只显示自己的进程 */
    if (page->view == PROC_VIEW_SYSTEM) {
        if (!p->is_system)
            return FALSE;
    } else {
        if (p->is_system)
            return FALSE;
        if (!page->is_root && p->uid != (guint32)page->uid)
            return FALSE;
    }

    const char *needle = page->search_text;
    if (needle == NULL || needle[0] == '\0')
        return TRUE;

    gchar *hay = g_strdup_printf("%u %u %u %s %s %c", p->pid, p->ppid,
                                 p->threads,
                                 (p->user != NULL) ? p->user : "",
                                 (p->comm != NULL) ? p->comm : "", p->state);
    gchar *lower_hay    = g_utf8_strdown(hay, -1);
    gchar *lower_needle = g_utf8_strdown(needle, -1);
    gboolean found      = (strstr(lower_hay, lower_needle) != NULL);

    g_free(hay);
    g_free(lower_hay);
    g_free(lower_needle);
    return found;
}

int proc_compare(gconstpointer a, gconstpointer b, gpointer data)
{
    const ProcItem *pa  = a;
    const ProcItem *pb  = b;
    int             col = GPOINTER_TO_INT(data);

    if (col < 0) {
        /* 默认排序：桌面应用 → 其他用户进程 → 系统/桌面组件，
         * 同组内按 CPU 降序，避免系统组件混在常用软件里被误杀。 */
        if (pa->category != pb->category)
            return (pa->category > pb->category) ? 1 : -1;
        if (pa->cpu != pb->cpu)
            return (pa->cpu < pb->cpu) ? 1 : -1;
        return 0;
    }

    switch (col) {
    case PROC_COL_PID:
        return (pa->pid > pb->pid) - (pa->pid < pb->pid);
    case PROC_COL_CATEGORY:
        return (pa->category > pb->category) - (pa->category < pb->category);
    case PROC_COL_USER:
        return g_strcmp0(pa->user, pb->user);
    case PROC_COL_STATE:
        return (pa->state > pb->state) - (pa->state < pb->state);
    case PROC_COL_CPU:
        return (pa->cpu > pb->cpu) - (pa->cpu < pb->cpu);
    case PROC_COL_MEM:
        return (pa->rss_kb > pb->rss_kb) - (pa->rss_kb < pb->rss_kb);
    case PROC_COL_TIME:
        return (pa->cputime_s > pb->cputime_s) -
               (pa->cputime_s < pb->cputime_s);
    case PROC_COL_SWAP:
        return (pa->swap_kb > pb->swap_kb) - (pa->swap_kb < pb->swap_kb);
    case PROC_COL_PPID:
        return (pa->ppid > pb->ppid) - (pa->ppid < pb->ppid);
    case PROC_COL_THREADS:
        return (pa->threads > pb->threads) - (pa->threads < pb->threads);
    case PROC_COL_COMM:
        return g_strcmp0(pa->comm, pb->comm);
    default:
        return 0;
    }
}
