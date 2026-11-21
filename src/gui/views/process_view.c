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
#include "process_tree.h"
#include "process_export.h"
#include "process_hint.h"
#include "process_scroll.h"
#include "process_selection.h"

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


/* 重建模型前记住选中的 pid：手动刷新不应把用户的操作对象弄丢 */
static void on_ps_done(gpointer user_data, const FmonChildResult *res,
                       gboolean ok)
{
    ProcPage *page = user_data;

    page->busy = FALSE;

    if (ok && res != NULL && res->out != NULL) {
        GPtrArray *rows = g_ptr_array_new_with_free_func(proc_ps_row_free);
        guint64    fp   = proc_parse_ps_rows(res->out, rows);

        if (fp != page->fingerprint) {
            guint32 keep_pid  = proc_selection_selected_pid(page);
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
            proc_selection_schedule_restore(page, keep_pid, keep_vpos);
        }

        g_ptr_array_free(rows, TRUE);
        proc_page_update_hint(page);
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
            proc_selection_select(page, i);
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
    proc_page_update_hint(page);

    /* 切换视图后立刻按新数据源刷新一次（ps 参数不同） */
    start_refresh(page);
    proc_view_scroll_top(page);
}


/* ===========================================================================
 * 导出进程列表
 * ========================================================================= */
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
static void on_page_mapped(GtkWidget *widget, gpointer data)
{
    (void)widget;

    ProcPage *page = data;

    start_refresh(page);
    proc_view_scroll_top(page);
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

static void on_end_task_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    ProcPage *page = data;
    ProcItem *item = proc_selection_get_item(page);

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

    ProcItem *item = proc_selection_get_item(data);

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
    g_signal_connect(btn_tree, "toggled", G_CALLBACK(proc_tree_toggled), page);
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
                                  "fmon-menu-symbolic");
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
                         G_CALLBACK(proc_tree_sorter_changed), page);

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
                     G_CALLBACK(proc_selection_changed), page);
    g_signal_connect(page->btn_end, "clicked",
                     G_CALLBACK(on_end_task_clicked), page);
    g_signal_connect(page->btn_details, "clicked",
                     G_CALLBACK(on_details_clicked), page);

    g_object_set_data_full(G_OBJECT(vbox), "proc-page", page,
                           proc_page_free);
    proc_page_update_hint(page);

    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     page);
    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_search_changed), page);
    g_signal_connect(item_export, "clicked", G_CALLBACK(proc_export_clicked),
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
