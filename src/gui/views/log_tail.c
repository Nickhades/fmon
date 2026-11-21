/*
 * log_tail.c - 日志增量读取与文件监听
 */

#include "log_tail.h"

#include "log_tail.h"
#include "log_internal.h"
#include "log_render.h"

#include "fmon_child.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void on_tail_done(gpointer user_data, const FmonChildResult *res,
                         gboolean ok)
{
    LogPage *page = user_data;

    page->busy = FALSE;

    /* 源已切换：丢弃旧结果，并立即按新源重新加载 */
    if (page->req_gen != page->generation) {
        /* 先启动新请求（它会再次持有 root 引用），再释放本次引用 */
        log_tail_start(page);
        g_object_unref(page->root);
        return;
    }

    /* 回调期间我们持有 root 的引用，因此 page 与控件一定存活 */

    gboolean was_at_bottom = log_render_at_bottom(page);

    gchar *raw = NULL;
    if (ok && res != NULL && res->out != NULL)
        raw = fmon_gui_make_valid_utf8(res->out, (gssize)res->out_len);
    else
        raw = g_strdup("");

    if (page->raw == NULL)
        page->raw = g_string_new(NULL);

    if (page->pending == NULL)
        page->pending = g_string_new(NULL);

    if (!page->append) {
        g_string_truncate(page->raw, 0);
        g_string_truncate(page->pending, 0);
    }

    /* 把新字节接到上次残留的半行后面，只把完整行放进 raw */
    g_string_append(page->pending, raw);

    char *last_nl = strrchr(page->pending->str, '\n');

    if (last_nl != NULL) {
        gsize complete = (gsize)(last_nl - page->pending->str) + 1;

        g_string_append_len(page->raw, page->pending->str, complete);
        g_string_erase(page->pending, 0, complete);
    }

    page->append = FALSE;

    /* 控制内存：只保留最后约 1 MB */
    if (page->raw->len > 8 * 1024 * 1024) {
        g_string_erase(page->raw, 0, page->raw->len - 8 * 1024 * 1024);

        /* 从整行边界重新开始，避免上下文首行是半行 */
        char *nl = strchr(page->raw->str, '\n');

        if (nl != NULL)
            g_string_erase(page->raw, 0,
                           (gsize)(nl - page->raw->str) + 1);
    }

    GString *filtered = log_render_filter(page, page->raw->str);
    g_free(raw);

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *current = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (g_strcmp0(current, filtered->str) != 0) {
        /* 保留滚动位置：只有原本在底部、且处于“继续”状态时才自动滚到底 */
        gtk_text_buffer_set_text(buffer, filtered->str, -1);
        log_render_apply_pid_tags(buffer);

        if (page->follow && was_at_bottom) {
            GtkTextIter end_iter;
            gtk_text_buffer_get_end_iter(buffer, &end_iter);
            gtk_text_buffer_place_cursor(buffer, &end_iter);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(page->text_view),
                                         &end_iter, 0.0, FALSE, 0, 0);
        }
    }

    g_free(current);
    g_string_free(filtered, TRUE);

    /* 释放 start_tail() 取得的引用 */
    g_object_unref(page->root);
}

static void on_file_changed(GFileMonitor *mon, GFile *file, GFile *other,
                            GFileMonitorEvent event, gpointer data)
{
    (void)mon;
    (void)file;
    (void)other;

    LogPage *page = data;

    if (event == G_FILE_MONITOR_EVENT_CHANGED ||
        event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event == G_FILE_MONITOR_EVENT_CREATED)
        log_tail_start(page);
}

void log_tail_setup_monitor(LogPage *page)
{
    if (page->monitor != NULL) {
        g_file_monitor_cancel(page->monitor);
        g_clear_object(&page->monitor);
    }

    if (page->path == NULL || page->cmd != NULL ||
        !g_file_test(page->path, G_FILE_TEST_IS_REGULAR))
        return;

    GFile *f = g_file_new_for_path(page->path);

    page->monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);

    if (page->monitor != NULL)
        g_signal_connect(page->monitor, "changed",
                         G_CALLBACK(on_file_changed), page);

    g_object_unref(f);
}

void log_tail_start(LogPage *page)
{
    if (page->busy)
        return;

    page->busy = TRUE;
    page->req_gen = page->generation;

    if (page->cmd != NULL) {
        const char *argv[] = { "/bin/sh", "-c", page->cmd, NULL };

        page->append = FALSE;
        page->offset = 0;
        g_object_ref(page->root);
        fmon_gui_run_async_full(argv, 4 * 1024 * 1024, 8000, on_tail_done,
                                page);
    } else {
        struct stat st;

        if (page->path == NULL || stat(page->path, &st) != 0) {
            page->busy = FALSE;
            return;
        }

        goffset size = (goffset)st.st_size;

        /* 原地重写但大小不变时，靠 mtime 使上下文缓存失效 */
        if (page->ctx_cache != NULL && page->initialized &&
            (st.st_mtime != page->file_mtime || size < page->offset))
            g_hash_table_remove_all(page->ctx_cache);

        page->file_mtime = st.st_mtime;

        if (!page->initialized || size < page->offset) {
            if (page->ctx_cache != NULL && size < page->offset)
                g_hash_table_remove_all(page->ctx_cache);
            /* 首次加载或文件被轮转：读最后 400 行 */
            const char *argv[] = { "tail", "-n", "400", page->path, NULL };

            page->append = FALSE;
            page->initialized = TRUE;
            g_object_ref(page->root);
            fmon_gui_run_async_full(argv, 2 * 1024 * 1024, 5000,
                                    on_tail_done, page);
            page->offset = size;
        } else if (size > page->offset) {
            /* 真正的增量：只读新追加的字节 */
            gchar *from = g_strdup_printf("+%" G_GINT64_FORMAT,
                                          (gint64)page->offset + 1);
            const char *argv[] = { "tail", "-c", from, page->path, NULL };

            page->append = TRUE;
            g_object_ref(page->root);
            fmon_gui_run_async_full(argv, 2 * 1024 * 1024, 5000,
                                    on_tail_done, page);
            g_free(from);
            page->offset = size;
        } else {
            page->busy = FALSE;
            return;   /* 没有新内容 */
        }
    }

    gchar *label = g_strdup_printf(_("日志源：%s"),
                                   (page->cmd != NULL) ? page->cmd
                                                       : page->path);
    gtk_label_set_text(GTK_LABEL(page->path_label), label);
    g_free(label);
}

/* ===========================================================================
 * 定时器
 * ========================================================================= */
