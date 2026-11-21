/*
 * log_view.c - 系统日志查看器
 *
 * 旧版本的问题：
 *   1. 每 3 秒在 GTK 主线程里 popen("tail -n 100 ...")，读取时界面会卡。
 *   2. 定时器永远不会停止：窗口关闭后回调仍然执行，而 g_log_text_view
 *      已经被销毁 —— 这是一个典型的 use-after-free（GTK4 的普通控件
 *      没有 destroy 信号，必须在控件销毁前主动移除 source）。
 *   3. 只能看 /var/log/messages，不能搜索、不能暂停、不能过滤。
 *
 * 现在：
 *   - tail 在后台线程执行（GTask），主线程不阻塞；
 *   - 定时器通过 g_object_set_data_full 绑定控件生命周期，控件销毁即移除；
 *     异步回调期间对根控件加引用，回调返回后再释放；
 *   - 增加文件选择、搜索、级别过滤、暂停/继续、自动滚动。
 */

#include <glib/gi18n.h>

#include "log_view.h"

#include "log_filter.h"
#include "log_sources.h"
#include "log_internal.h"
#include "log_render.h"
#include "log_context.h"
#include "log_tail.h"

#include "gui_util.h"
#include "gui_window.h"
#include "process_view.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_MAX_LINES 2000







static void log_page_free(gpointer data);

/* ===========================================================================
 * 过滤
 * ========================================================================= */
static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);
    if (id != 0)
        g_source_remove(id);
}

static gboolean on_tick(gpointer data)
{
    LogPage *page = data;

    if (fmon_gui_main_window() == NULL || page->root == NULL)
        return G_SOURCE_REMOVE;

    if (!gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    if (page->follow)
        log_tail_start(page);

    return G_SOURCE_CONTINUE;
}

/* ===========================================================================
 * 交互
 * ========================================================================= */
static void on_path_selected(const char *path, LogPage *page)
{
    g_free(page->path);
    g_free(page->cmd);
    page->cmd = NULL;
    page->generation++;
    page->path = g_strdup(path);
    page->offset = 0;
    page->initialized = FALSE;
    if (page->raw != NULL)
        g_string_truncate(page->raw, 0);
    if (page->pending != NULL)
        g_string_truncate(page->pending, 0);
    if (page->ctx_cache != NULL)
        g_hash_table_remove_all(page->ctx_cache);

    log_tail_setup_monitor(page);

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));
    gtk_text_buffer_set_text(buffer, "", 0);

    log_tail_start(page);
}

static void on_preset_changed(GObject *dropdown, GParamSpec *pspec,
                              gpointer data)
{
    (void)pspec;

    LogPage *page = data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));

    if (sel == GTK_INVALID_LIST_POSITION)
        return;

    if (sel >= page->sources->len)
        return;   /* 最后一项“自定义…”由按钮处理 */

    LogSource *src = g_ptr_array_index(page->sources, sel);

    g_free(page->path);
    g_free(page->cmd);
    page->path = g_strdup(src->path);
    page->cmd = g_strdup(src->cmd);
    page->generation++;
    page->offset = 0;
    page->initialized = FALSE;
    if (page->raw != NULL)
        g_string_truncate(page->raw, 0);
    if (page->pending != NULL)
        g_string_truncate(page->pending, 0);
    if (page->ctx_cache != NULL)
        g_hash_table_remove_all(page->ctx_cache);

    log_tail_setup_monitor(page);

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));
    gtk_text_buffer_set_text(buffer, "", 0);
    log_tail_start(page);
}

static void on_open_file_done(GObject *source_object, GAsyncResult *result,
                              gpointer user_data)
{
    LogPage *page = user_data;

    if (page->root == NULL)
        return;

    GError *error = NULL;
    GFile  *file  = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object),
                                                result, &error);

    if (file == NULL) {
        if (error != NULL)
            g_error_free(error);
        return;
    }

    gchar *path = g_file_get_path(file);
    if (path != NULL) {
        on_path_selected(path, page);
        g_free(path);
    }
    g_object_unref(file);
}

static void on_open_file(GtkButton *btn, gpointer data)
{
    (void)btn;

    LogPage *page = data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "打开日志文件");
    gtk_file_dialog_set_initial_folder(
        dialog, g_file_new_for_path("/var/log"));

    g_object_ref(dialog);
    gtk_file_dialog_open(dialog, fmon_gui_main_window(), NULL,
                         on_open_file_done, page);
    g_object_unref(dialog);
}

static void on_export_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkTextView *tv = data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);

    if (file == NULL)
        return;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
    GtkTextIter s, e;

    gtk_text_buffer_get_bounds(buf, &s, &e);

    gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    gchar *path = g_file_get_path(file);

    if (path != NULL) {
        g_file_set_contents(path, text, -1, NULL);
        g_free(path);
    }

    g_free(text);
    g_object_unref(file);
}

static void on_export(GtkButton *btn, gpointer data)
{
    (void)btn;

    GtkFileDialog *dlg = gtk_file_dialog_new();

    gtk_file_dialog_set_title(dlg, _("导出当前日志"));
    gtk_file_dialog_set_initial_name(dlg, "fmon-log.txt");
    g_object_ref(dlg);
    gtk_file_dialog_save(dlg, fmon_gui_main_window(), NULL, on_export_done,
                         data);
    g_object_unref(dlg);
}

static void on_reload(GtkButton *btn, gpointer data)
{
    (void)btn;

    LogPage *page = data;

    page->offset = 0;
    page->initialized = FALSE;
    if (page->raw != NULL)
        g_string_truncate(page->raw, 0);
    if (page->pending != NULL)
        g_string_truncate(page->pending, 0);
    log_tail_start(page);
}

static void on_follow_toggled(GtkToggleButton *btn, gpointer data)
{
    LogPage *page = data;

    page->follow = gtk_toggle_button_get_active(btn);
    gtk_button_set_label(GTK_BUTTON(btn), page->follow ? "暂停" : "继续");

    if (page->follow)
        log_tail_start(page);
}

static void on_filter_changed(GObject *dropdown, GParamSpec *pspec,
                              gpointer data)
{
    (void)dropdown;
    (void)pspec;
    (void)data;

    /* 有累积数据就直接重过滤，暂停状态下也要立即生效 */
    LogPage *page = data;

    if (page->raw != NULL && page->raw->len > 0)
        log_render_refilter(page);
    else
        log_tail_start(page);
}

/* ===========================================================================
 * 日志源
 * ========================================================================= */
static void log_page_free(gpointer data)
{
    LogPage *page = data;

    if (page == NULL)
        return;

    g_free(page->path);
    g_free(page->cmd);
    if (page->raw != NULL)
        g_string_free(page->raw, TRUE);
    if (page->pending != NULL)
        g_string_free(page->pending, TRUE);
    if (page->ctx_cache != NULL)
        g_hash_table_destroy(page->ctx_cache);
    if (page->monitor != NULL) {
        g_signal_handlers_disconnect_by_data(page->monitor, page);
        g_file_monitor_cancel(page->monitor);
        g_object_unref(page->monitor);
    }
    if (page->sources != NULL)
        g_ptr_array_free(page->sources, TRUE);
    g_free(page);
}

GtkWidget *gui_log_view_new(void)
{
    LogPage *page = g_new0(LogPage, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(vbox, "view-card");
    gtk_widget_set_vexpand(vbox, TRUE);
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *filterbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_reload = gtk_button_new_with_label(_("刷新"));
    gtk_widget_add_css_class(btn_reload, "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), btn_reload);

    GtkWidget *btn_open = gtk_button_new_with_label(_("打开文件…"));
    gtk_box_append(GTK_BOX(toolbar), btn_open);

    page->sources = log_sources_build();

    GtkStringList *presets = gtk_string_list_new(NULL);

    for (guint i = 0; i < page->sources->len; i++) {
        LogSource *s = g_ptr_array_index(page->sources, i);

        gtk_string_list_append(presets, s->label);
    }

    gtk_string_list_append(presets, _("自定义…"));

    page->preset_dd = gtk_drop_down_new(G_LIST_MODEL(presets), NULL);
    g_object_unref(presets);
    gtk_box_append(GTK_BOX(toolbar), page->preset_dd);

    GtkStringList *levels = gtk_string_list_new(NULL);
    gtk_string_list_append(levels, g_strdup(fmon_i18n_tr("全部级别")));
    gtk_string_list_append(levels, "紧急");
    gtk_string_list_append(levels, "警报");
    gtk_string_list_append(levels, "严重");
    gtk_string_list_append(levels, "错误");
    gtk_string_list_append(levels, "警告");
    gtk_string_list_append(levels, "通知");
    gtk_string_list_append(levels, "信息");
    gtk_string_list_append(levels, "调试");

    page->level_dd = gtk_drop_down_new(G_LIST_MODEL(levels), NULL);
    g_object_unref(levels);
    gtk_box_append(GTK_BOX(toolbar), page->level_dd);

    GtkStringList *ranges = gtk_string_list_new(NULL);
    gtk_string_list_append(ranges, g_strdup(fmon_i18n_tr("全部时间")));
    gtk_string_list_append(ranges, "1 小时");
    gtk_string_list_append(ranges, "6 小时");
    gtk_string_list_append(ranges, "24 小时");
    page->range_dd = gtk_drop_down_new(G_LIST_MODEL(ranges), NULL);
    g_object_unref(ranges);
    gtk_box_append(GTK_BOX(toolbar), page->range_dd);

    page->search = gtk_search_entry_new();
    gtk_widget_set_hexpand(page->search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->search),
                                           _("搜索（支持 -排除词）"));
    gtk_widget_set_size_request(page->search, 220, -1);
    gtk_box_append(GTK_BOX(filterbar), page->search);

    page->exclude = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->exclude),
                                           _("排除关键词"));
    gtk_widget_set_size_request(page->exclude, 140, -1);
    gtk_box_append(GTK_BOX(filterbar), page->exclude);

    page->regex_btn = gtk_toggle_button_new_with_label(".*");
    gtk_widget_set_tooltip_text(page->regex_btn, _("按正则表达式匹配"));
    gtk_box_append(GTK_BOX(filterbar), page->regex_btn);

    page->dedup_btn = gtk_toggle_button_new_with_label(_("折叠重复"));
    gtk_box_append(GTK_BOX(filterbar), page->dedup_btn);

    page->follow_btn = gtk_toggle_button_new_with_label(_("暂停"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->follow_btn), TRUE);
    gtk_box_append(GTK_BOX(toolbar), page->follow_btn);

    GtkWidget *btn_export = gtk_button_new_with_label(_("导出"));

    gtk_box_append(GTK_BOX(toolbar), btn_export);

    gtk_box_append(GTK_BOX(vbox), toolbar);
    gtk_box_append(GTK_BOX(vbox), filterbar);

    page->path_label = gtk_label_new(_("日志文件：/var/log/messages"));
    gtk_widget_add_css_class(page->path_label, "small-text");
    gtk_widget_set_halign(page->path_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), page->path_label);

    page->scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(page->scroll, TRUE);

    page->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(page->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(page->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(page->text_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start(page->text_view, 8);
    gtk_widget_set_margin_end(page->text_view, 8);
    gtk_widget_set_margin_top(page->text_view, 8);
    gtk_widget_set_margin_bottom(page->text_view, 8);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(page->scroll),
                                  page->text_view);
    gtk_box_append(GTK_BOX(vbox), page->scroll);

    GtkGesture *click = gtk_gesture_click_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "pressed", G_CALLBACK(log_context_pid_click), page);
    gtk_widget_add_controller(page->text_view,
                              GTK_EVENT_CONTROLLER(click));

    page->path     = g_strdup("/var/log/messages");
    page->follow   = TRUE;
    log_tail_setup_monitor(page);

    g_object_set_data_full(G_OBJECT(vbox), "log-page", page, log_page_free);

    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload), page);
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_file), page);
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_export),
                     page->text_view);
    g_signal_connect(page->preset_dd, "notify::selected",
                     G_CALLBACK(on_preset_changed), page);
    g_signal_connect(page->exclude, "search-changed",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->regex_btn, "toggled",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->dedup_btn, "toggled",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->level_dd, "notify::selected",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->range_dd, "notify::selected",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->follow_btn, "toggled",
                     G_CALLBACK(on_follow_toggled), page);

    /* 定时器与控件同生命周期：控件销毁时自动移除 */
    page->timer = g_timeout_add_seconds(3, on_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "log-timer",
                           GUINT_TO_POINTER(page->timer), remove_source);

    log_tail_start(page);

    return vbox;
}
