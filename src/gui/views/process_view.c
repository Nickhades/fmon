/*
 * process_view.c - 进程管理
 *
 * 旧版本的问题：
 *   1. 命令统一通过 /bin/sh -c "..." 执行，用户名/命令串里只要出现
 *      $()、;、反引号就可能被 shell 解释（命令注入）。
 *   2. 刷新在 GTK 主线程里同步 popen("ps ... | tail")，进程多时界面卡住。
 *   3. “结束进程”直接 pkexec kill -9，没有二次确认，误点即杀。
 *   4. 用 ps 输出里的 "sh -c"、"grep " 之类的字符串来判断并过滤“自己启动
 *      的临时进程”，会误伤真实进程；实际上只要去掉 `| tail` 管道、跳过
 *      表头行就不需要这个黑名单。
 *   5. 没有自动刷新、不能排序、没有信号选择。
 *
 * 现在：
 *   - ps 通过 argv 执行，跳过表头，不再使用 shell；
 *   - 后台线程刷新 + JSON 无关的简单文本解析；
 *   - 列头可排序（GtkSortListModel + GtkCustomSorter）；
 *   - 结束/暂停/恢复/优先级/详情/lsof/sockstat，全部带确认或异步；
 *   - 同用户进程直接调用 kill(2)，其他用户进程才走 pkexec；
 *   - 自动刷新在用户选中某一行时暂停，避免打断操作。
 */

#include <glib/gi18n.h>

#include "process_internal.h"

#include "cpuset_dialog.h"
#include "gui_ctl.h"
#include "gui_util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

static ProcPage *g_proc_page;

static void update_hint(ProcPage *page);
static void proc_scroll_top(ProcPage *page);
static void on_selection_changed(GtkSelectionModel *sel, guint position,
                                 guint n_items, gpointer data);

/* 重建模型前记住选中的 pid：手动刷新不应把用户的操作对象弄丢 */
static guint sel_first_pos(ProcPage *page)
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

static void sel_select_one(ProcPage *page, guint pos)
{
    GtkSelectionModel *m = GTK_SELECTION_MODEL(page->selection);

    gtk_selection_model_unselect_all(m);

    if (pos != GTK_INVALID_LIST_POSITION)
        gtk_selection_model_select_item(m, pos, TRUE);
}

static guint32 selected_pid(ProcPage *page)
{
    guint pos = sel_first_pos(page);

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
            sel_select_one(page, i);
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
        sel_select_one(ctx->page, GTK_INVALID_LIST_POSITION);

    on_selection_changed(GTK_SELECTION_MODEL(ctx->page->selection), 0, 0,
                         ctx->page);

    if (vadj != NULL) {
        gtk_adjustment_set_value(vadj, ctx->vpos);
        g_timeout_add(40, enforce_scroll_cb, ctx);
        return G_SOURCE_REMOVE;
    }

    g_free(ctx);
    return G_SOURCE_REMOVE;
}

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

static void proc_tree_sort_items(ProcItem **items, guint n, ProcPage *page)
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

    for (guint i = 0; i < n; i++)
        items[i] = g_array_index(ctx.flat, ProcItem *, i);

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

static void on_view_sorter_changed(GtkSorter *sorter, GParamSpec *pspec,
                                  gpointer data)
{
    (void)pspec;

    ProcPage *page = data;

    if (page->tree_mode)
        return;

    gtk_sort_list_model_set_sorter(page->sort_model, sorter);
}

static void on_tree_toggled(GtkToggleButton *btn, gpointer data)
{
    ProcPage *page = data;

    page->tree_mode = gtk_toggle_button_get_active(btn);

    if (page->tree_mode) {
        gtk_sort_list_model_set_sorter(page->sort_model, NULL);
        proc_scroll_top(page);

        guint     n   = g_list_model_get_n_items(G_LIST_MODEL(page->store));
        ProcItem **arr = g_new0(ProcItem *, n);

        for (guint i = 0; i < n; i++)
            arr[i] = g_list_model_get_item(G_LIST_MODEL(page->store), i);

        proc_tree_sort_items(arr, n, page);

        g_list_store_remove_all(page->store);
        g_list_store_splice(page->store, 0, 0, (gpointer *)arr, n);

        for (guint i = 0; i < n; i++)
            g_object_unref(arr[i]);
        g_free(arr);
    } else {
        GtkCustomSorter *s = gtk_custom_sorter_new(proc_compare,
                                                   GINT_TO_POINTER(-1), NULL);

        gtk_sort_list_model_set_sorter(page->sort_model, GTK_SORTER(s));
    }
}

/* ps 异步完成：解析 → 指纹比较 → 重建 GListStore */
static void on_ps_done(gpointer user_data, const FmonChildResult *res,
                       gboolean ok)
{
    ProcPage *page = user_data;

    page->busy = FALSE;

    if (ok && res != NULL && res->out != NULL) {
        GPtrArray *rows = g_ptr_array_new_with_free_func(proc_ps_row_free);
        guint64    fp   = proc_parse_ps_rows(res->out, rows);

        if (fp != page->fingerprint) {
            guint32 keep_pid  = selected_pid(page);
            double  keep_vpos = 0.0;

            if (page->scroll != NULL)
                keep_vpos = gtk_adjustment_get_value(
                    gtk_scrolled_window_get_vadjustment(
                        GTK_SCROLLED_WINDOW(page->scroll)));

            guint     n_old = g_list_model_get_n_items(G_LIST_MODEL(page->store));
            GObject **items = g_new0(GObject *, rows->len + 1);

            page->fingerprint = fp;

            for (guint i = 0; i < rows->len; i++) {
                PsRow    *r = g_ptr_array_index(rows, i);
                ProcItem *item = g_object_new(proc_item_get_type(), NULL);

                item->pid       = r->pid;
                item->uid       = r->uid;
                item->state     = r->state;
                item->user      = g_strdup(r->user);
                item->comm      = g_strdup(r->comm);
                item->cpu       = r->cpu;

                /* 有 top 的 WCPU 就用它（两位小数，空闲进程不会全变 0.0） */
                if (page->cpu_map != NULL) {
                    gdouble *wcpu =
                        g_hash_table_lookup(page->cpu_map, &r->pid);

                    if (wcpu != NULL)
                        item->cpu = *wcpu;
                }
                item->mem       = r->mem;
                item->rss_kb    = r->rss_kb;
                item->is_system = r->is_system;

                if (page->extra_map != NULL) {
                    ProcExtra *ex = g_hash_table_lookup(page->extra_map,
                                                        &r->pid);

                    if (ex != NULL) {
                        item->ppid      = ex->ppid;
                        item->swap_kb   = ex->swap_kb;
                        item->cputime_s = ex->cputime_s;
                        item->threads   = ex->threads;

                        if (ex->rss_kb > 0)
                            item->rss_kb = ex->rss_kb;
                    }
                }
                item->category  = proc_classify_category(r->comm, r->uid,
                                                         r->is_system);

                items[i] = G_OBJECT(item);
            }


            /* 一次性替换整个模型：逐条 append 会产生 N 次
             * items-changed，过滤/排序/选择模型每收到一次都要做一轮
             * 增量更新（几百行时单次刷新要几十毫秒）。splice 只发一次信号。 */
            if (page->tree_mode)
                proc_tree_sort_items((ProcItem **)items, rows->len, page);

            g_list_store_splice(page->store, 0, n_old, (gpointer *)items,
                                rows->len);

            for (guint i = 0; i < rows->len; i++)
                g_object_unref(items[i]);
            g_free(items);

            /* 数据变了要重建；选中行与滚动位置在 idle 里恢复，
             * 避免按旧索引选中错误行、或让视口跳到选中行。 */
            if (keep_pid != 0) {
                RestoreCtx *ctx = g_new0(RestoreCtx, 1);

                ctx->page = page;
                ctx->pid  = keep_pid;
                ctx->vpos = keep_vpos;
                g_idle_add(restore_selection_cb, ctx);
            }
        }

        g_ptr_array_free(rows, TRUE);
        update_hint(page);
    }

    if (page->root != NULL)
        g_object_unref(page->root);
}

static void start_refresh(ProcPage *page)
{
    if (page->busy || page->root == NULL)
        return;

    page->busy = TRUE;

    /* ps 的 pcpu 只打印一位小数，空闲进程全被抹成 0.0；改用内核的
     * ki_pctcpu（与 top 的 WCPU 同源）补上精度。一次 sysctl 约 0.7 ms，
     * 取不到时 cpu_map 为 NULL，on_ps_done 会回退到 ps 的 pcpu。 */
    g_clear_pointer(&page->cpu_map, g_hash_table_destroy);
    page->cpu_map = proc_ps_read_wcpu();

    g_clear_pointer(&page->extra_map, g_hash_table_destroy);
    page->extra_map = proc_ps_read_extras();

    /* 优先用 libprocstat 直接读取全部进程；不再 fork ps。
     * 只有 procstat 不可用时才回退到旧的 ps 路径。 */
    gchar *dump = proc_read_libprocstat_text();

    if (dump != NULL) {
        FmonChildResult fake;

        memset(&fake, 0, sizeof(fake));
        fake.out     = dump;
        fake.out_len = strlen(dump);

        /* on_ps_done 末尾会 unref root（异步路径在发起前 ref 过），
         * 同步路径必须同样先持有一个引用，否则窗口会被提前 finalize。 */
        g_object_ref(page->root);
        on_ps_done(page, &fake, TRUE);
        g_free(dump);
        return;
    }

    /* LC_NUMERIC=C 保证 pcpu/pmem 的小数点是 '.'，与 sscanf("%lf") 一致；
     * 不经过 shell，因此不需要任何引号处理。
     *
     * 普通用户只看自己的进程（ps -U <uid>），行数从上千降到几十，
     * 解析和列表重建的开销也随之下降两个数量级。 */
    static char uid_str[16];

    const char *argv[12];
    int         n = 0;

    argv[n++] = "env";
    argv[n++] = "LC_NUMERIC=C";
    argv[n++] = "ps";

    /* 普通用户看“系统”视图时也要列出全部进程（只读）；
     * 只有“普通用户”视图才用 -U 限制成自己的进程。 */
    if (!page->is_root && page->view == PROC_VIEW_USER) {
        g_snprintf(uid_str, sizeof(uid_str), "%u", (unsigned)page->uid);
        argv[n++] = "-U";
        argv[n++] = uid_str;
    } else {
        argv[n++] = "-ax";
    }

    argv[n++] = "-ww";
    argv[n++] = "-o";
    argv[n++] = "pid,uid,user,pcpu,pmem,rss,stat,command";
    argv[n]   = NULL;

    g_object_ref(page->root);
    fmon_gui_run_async_full(argv, 4 * 1024 * 1024, 5000, on_ps_done, page);
}

/* ===========================================================================
 * 过滤器 / 排序
 * ========================================================================= */


static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    ProcPage *page = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    g_free(page->search_text);
    page->search_text = g_strdup(text);
    proc_list_apply_filter(page);
}

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
                    _("%s（已选中 %" G_GUINT64_FORMAT " 个进程，自动刷新已暂停）"),
                    text, nsel);
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

gboolean gui_process_select_pid(guint32 pid)
{
    ProcPage *page = g_proc_page;

    if (page == NULL || page->selection == NULL)
        return FALSE;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(page->selection));

    for (guint i = 0; i < n; i++) {
        ProcItem *it = g_list_model_get_item(G_LIST_MODEL(page->selection), i);

        if (it == NULL)
            continue;

        gboolean match = (it->pid == pid);

        g_object_unref(it);

        if (match) {
            sel_select_one(page, i);
            return TRUE;
        }
    }

    return FALSE;
}

static void on_view_toggled(GtkToggleButton *btn, gpointer data)
{
    ProcPage *page = data;

    if (!gtk_toggle_button_get_active(btn))
        return;

    ProcView view = (btn == GTK_TOGGLE_BUTTON(page->btn_system))
                        ? PROC_VIEW_SYSTEM
                        : PROC_VIEW_USER;

    if (page->view == view)
        return;

    page->view = view;
    proc_list_apply_filter(page);
    update_hint(page);

    /* 切换视图后立刻按新数据源刷新一次（ps 参数不同） */
    start_refresh(page);
    proc_scroll_top(page);
}


/* ===========================================================================
 * 导出进程列表
 * ========================================================================= */
static void on_export_done(GObject *source_object, GAsyncResult *result,
                           gpointer user_data)
{
    GtkFilterListModel *filter_model = user_data;
    GError   *error = NULL;

    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object),
                                              result, &error);

    if (file == NULL) {
        if (error != NULL) {
            if (!g_error_matches(error, GTK_DIALOG_ERROR,
                                 GTK_DIALOG_ERROR_DISMISSED))
                fmon_gui_show_error(fmon_gui_main_window(), _("无法保存文件"),
                                    error->message);
            g_error_free(error);
        }
        g_object_unref(filter_model);
        return;
    }

    gchar *path = g_file_get_path(file);

    if (path == NULL) {
        fmon_gui_show_error(fmon_gui_main_window(), _("导出失败"), _("请选择本地文件系统上的位置。"));
        g_object_unref(file);
        return;
    }

    FILE *fp = fopen(path, "wb");

    if (fp == NULL) {
        gchar *msg = g_strdup_printf(_("无法写入 %s：%s"), path,
                                     g_strerror(errno));
        fmon_gui_show_error(fmon_gui_main_window(), _("导出失败"), msg);
        g_free(msg);
        g_free(path);
        g_object_unref(file);
        g_object_unref(filter_model);
        return;
    }

    fputs("PID,User,System,State,CPU(%),MEM(RSS KB),MEM(%),Command\n", fp);

    guint n = g_list_model_get_n_items(G_LIST_MODEL(filter_model));

    for (guint i = 0; i < n; i++) {
        ProcItem *p = g_list_model_get_item(G_LIST_MODEL(filter_model), i);

        if (p == NULL)
            continue;

        gchar cpu[G_ASCII_DTOSTR_BUF_SIZE];
        gchar mem[G_ASCII_DTOSTR_BUF_SIZE];

        g_ascii_formatd(cpu, sizeof(cpu), "%.2f", p->cpu);
        g_ascii_formatd(mem, sizeof(mem), "%.1f", p->mem);

        fprintf(fp, "%u,%s,%s,%c,%s,%llu,%s,", p->pid,
                (p->user != NULL) ? p->user : "",
                p->is_system ? "system" : "user", p->state, cpu,
                (unsigned long long)p->rss_kb, mem);

        /* 命令行里可能包含逗号和双引号，按 CSV 规则转义 */
        const gchar *cmd = (p->comm != NULL) ? p->comm : "";
        fputc('"', fp);
        for (const gchar *c = cmd; *c != '\0'; c++) {
            if (*c == '"')
                fputc('"', fp);
            fputc(*c, fp);
        }
        fputs("\"\n", fp);

        g_object_unref(p);
    }

    fclose(fp);

    gchar *detail = g_strdup_printf(_("已导出 %u 行到：\n%s"), n, path);
    fmon_gui_show_info(fmon_gui_main_window(), "导出成功", detail);
    g_free(detail);
    g_free(path);
    g_object_unref(file);
    g_object_unref(filter_model);
}

static void on_export_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "保存进程列表 CSV");
    gtk_file_dialog_set_initial_name(dialog, "fmon_processes.csv");
    gtk_file_dialog_set_accept_label(dialog, "保存");

    ProcPage *page = data;

    g_object_ref(dialog);
    gtk_file_dialog_save(dialog, fmon_gui_main_window(), NULL, on_export_done,
                         g_object_ref(page->filter_model));
    g_object_unref(dialog);
}

/* ===========================================================================
 * 定时刷新
 * ========================================================================= */
static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);
    if (id != 0)
        g_source_remove(id);
}

static gboolean on_refresh_tick(gpointer data)
{
    ProcPage *page = data;

    if (fmon_gui_main_window() == NULL || page->root == NULL)
        return G_SOURCE_REMOVE;

    /* 页面不可见时不做任何工作：GTK4 的 GtkStack 只会 map 当前页，
     * 隐藏页面的定时器不必消耗 CPU（旧版本所有页面都在后台轮询）。 */
    if (!gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    start_refresh(page);
    return G_SOURCE_CONTINUE;
}

/* 切换页面/视图后把列表滚回顶部，避免切进来先看到下半截 */
static gboolean proc_scroll_top_cb(gpointer data)
{
    ProcPage *page = data;

    if (page->scroll != NULL && page->root != NULL &&
        gtk_widget_get_mapped(page->root))
        gtk_adjustment_set_value(
            gtk_scrolled_window_get_vadjustment(
                GTK_SCROLLED_WINDOW(page->scroll)),
            0.0);

    return G_SOURCE_REMOVE;
}

static void proc_scroll_top(ProcPage *page)
{
    if (page->scroll == NULL)
        return;

    /* 布局完成后 GtkListView 还会调整一次位置，延迟再补一次 */
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page->scroll)),
        0.0);
    g_timeout_add(60, proc_scroll_top_cb, page);
}

/* 切回本页时立刻刷新一次：隐藏页面的定时器不工作（见 on_refresh_tick），
 * 若不补一次，切回来可能先看到最多 3 秒前的旧数据。 */
static void on_page_mapped(GtkWidget *widget, gpointer data)
{
    (void)widget;

    ProcPage *page = data;

    start_refresh(page);
    proc_scroll_top(page);
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    start_refresh(data);
}

/* ===========================================================================
 * 页面
 * ========================================================================= */
static void proc_page_free(gpointer data)
{
    ProcPage *page = data;

    if (page == NULL)
        return;

    if (g_proc_page == page)
        g_proc_page = NULL;

    g_clear_pointer(&page->cpu_map, g_hash_table_destroy);
    g_clear_pointer(&page->extra_map, g_hash_table_destroy);
    g_free(page->search_text);
    g_free(page);
}

/* 底部操作栏用的辅助：取当前选中项 */
static ProcItem *get_selected_item(ProcPage *page)
{
    guint pos = sel_first_pos(page);

    if (pos == GTK_INVALID_LIST_POSITION)
        return NULL;

    return g_list_model_get_item(G_LIST_MODEL(page->selection), pos);
}

static void on_selection_changed(GtkSelectionModel *sel, guint position,
                                 guint n_items, gpointer data)
{
    (void)sel;
    (void)position;
    (void)n_items;

    ProcPage *page = data;
    gboolean  has  = sel_first_pos(page) !=
                    GTK_INVALID_LIST_POSITION;

    /* 普通用户只能操作自己的进程；root 进程只读。 */
    gboolean can_operate = has;

    if (has && geteuid() != 0) {
        ProcItem *item = get_selected_item(page);

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

    update_hint(page);
}

static void on_end_task_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    ProcPage *page = data;
    ProcItem *item = get_selected_item(page);

    if (item == NULL)
        return;

    SignalOp *op = proc_actions_signal_op_new(item, SIGTERM, TRUE);
    gchar    *msg = g_strdup_printf(_("结束进程 %u（%s）？"), item->pid,
                                    (item->comm != NULL) ? item->comm : "?");

    fmon_gui_confirm(fmon_gui_main_window(), msg,
                     "将先发送 SIGTERM 让进程正常退出。未保存的数据可能丢失。\n若进程属于其他用户，将弹出管理员认证窗口。",
                     "结束进程", proc_actions_send_signal, op,
                     (GDestroyNotify)proc_actions_signal_op_free);
    g_free(msg);
    g_object_unref(item);
}

static void on_details_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    ProcItem *item = get_selected_item(data);

    if (item == NULL)
        return;

    proc_windows_show_detail(fmon_gui_main_window(), item);
    g_object_unref(item);
}

GtkWidget *gui_process_view_new(void)
{
    ProcPage *page = g_new0(ProcPage, 1);

    g_proc_page = page;

    page->is_root     = (geteuid() == 0);
    page->uid         = getuid();
    page->view        = PROC_VIEW_USER;
    page->search_text = g_strdup("");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_add_css_class(vbox, "view-card");
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_refresh = gtk_button_new_with_label(_("刷新"));
    gtk_widget_add_css_class(btn_refresh, "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), btn_refresh);

    GtkWidget *btn_tree = gtk_toggle_button_new_with_label(_("进程树"));

    gtk_widget_set_tooltip_text(btn_tree,
                                _("按父子关系缩进显示；关闭后恢复扁平列表"));
    g_signal_connect(btn_tree, "toggled", G_CALLBACK(on_tree_toggled), page);
    gtk_box_append(GTK_BOX(toolbar), btn_tree);

    /* 两个视图按钮：普通用户 / 系统(root)。只有以 root 运行界面时才能看系统进程。 */
    GtkWidget *view_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(view_box, "linked");

    page->btn_user = gtk_toggle_button_new_with_label(_("普通用户"));
    page->btn_system = gtk_toggle_button_new_with_label(_("系统 (root)"));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(page->btn_system),
                                GTK_TOGGLE_BUTTON(page->btn_user));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->btn_user), TRUE);

    gtk_box_append(GTK_BOX(view_box), page->btn_user);
    gtk_box_append(GTK_BOX(view_box), page->btn_system);
    gtk_box_append(GTK_BOX(toolbar), view_box);

    if (!page->is_root) {
        /* 可查看、只读：普通用户能看 root 进程，但不能操作 */
        gtk_widget_set_tooltip_text(
            page->btn_system,
            _("查看系统进程（只读）；操作 root 进程需以 root 运行 fmon-gui"));
    }

    page->search = gtk_search_entry_new();
    gtk_widget_set_hexpand(page->search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->search), _("按进程名、PID、用户名过滤"));
    gtk_box_append(GTK_BOX(toolbar), page->search);

    /* 导出收进“⋮”菜单，窄窗口时工具栏不会挤 */
    GtkWidget *btn_menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn_menu),
                                  "open-menu-symbolic");
    gtk_widget_set_tooltip_text(btn_menu, _("更多操作"));

    GtkWidget *menu_pop = gtk_popover_new();
    GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *item_export = gtk_button_new_with_label(_("导出列表"));
    gtk_widget_add_css_class(item_export, "flat");

    gtk_box_append(GTK_BOX(menu_box), item_export);
    gtk_popover_set_child(GTK_POPOVER(menu_pop), menu_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn_menu), menu_pop);
    gtk_box_append(GTK_BOX(toolbar), btn_menu);

    gtk_box_append(GTK_BOX(vbox), toolbar);

    page->hint = gtk_label_new("");
    gtk_widget_add_css_class(page->hint, "small-text");
    gtk_widget_set_halign(page->hint, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(page->hint), TRUE);
    gtk_box_append(GTK_BOX(vbox), page->hint);

    GtkWidget *view = proc_list_build(page);

    /* 右键菜单 + 双击查看详情 */
    g_object_set_data(G_OBJECT(view), "proc-page", page);
    proc_actions_attach_view(view);

    /* ColumnView 的排序器不会自动套到 selection->sort 模型上，
     * 这里把表头排序器的变化手动转给 GtkSortListModel。 */
    GtkSorter *view_sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view));

    if (view_sorter != NULL)
        g_signal_connect(view_sorter, "changed",
                         G_CALLBACK(on_view_sorter_changed), page);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);
    page->scroll = scroll;

    /* Windows 11 风格底部操作栏 */
    GtkWidget *action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(action_bar, "action-bar");

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    page->btn_details = gtk_button_new_with_label(_("详细信息"));
    page->btn_end     = gtk_button_new_with_label(_("结束任务"));
    gtk_widget_add_css_class(page->btn_end, "destructive-action");
    gtk_widget_set_sensitive(page->btn_details, FALSE);
    gtk_widget_set_sensitive(page->btn_end, FALSE);
    gtk_widget_set_tooltip_text(page->btn_end, "结束选中的进程（SIGTERM）");

    gtk_box_append(GTK_BOX(action_bar), spacer);
    gtk_box_append(GTK_BOX(action_bar), page->btn_details);
    gtk_box_append(GTK_BOX(action_bar), page->btn_end);
    gtk_box_append(GTK_BOX(vbox), action_bar);

    g_signal_connect(page->selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), page);
    g_signal_connect(page->btn_end, "clicked",
                     G_CALLBACK(on_end_task_clicked), page);
    g_signal_connect(page->btn_details, "clicked",
                     G_CALLBACK(on_details_clicked), page);

    g_object_set_data_full(G_OBJECT(vbox), "proc-page", page,
                           proc_page_free);
    update_hint(page);

    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     page);
    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_search_changed), page);
    g_signal_connect(item_export, "clicked", G_CALLBACK(on_export_clicked),
                     page);
    g_signal_connect(page->btn_user, "toggled", G_CALLBACK(on_view_toggled),
                     page);
    g_signal_connect(page->btn_system, "toggled", G_CALLBACK(on_view_toggled),
                     page);

    g_signal_connect(vbox, "map", G_CALLBACK(on_page_mapped), page);

    guint timer = g_timeout_add_seconds(3, on_refresh_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "proc-timer", GUINT_TO_POINTER(timer),
                           remove_source);

    start_refresh(page);

    return vbox;
}
