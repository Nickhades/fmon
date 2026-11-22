/*
 * log_context.c - 日志上下文窗口与 PID 跳转
 */

#include "log_context.h"

#include "log_internal.h"
#include "log_render.h"

#include "gui_util.h"
#include "gui_window.h"
#include "process_view.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

static void show_context_window(const gchar *ctx)
{
    GtkWidget *win = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(win), _("日志上下文（前后 5 行）"));
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 320);
    gtk_window_set_transient_for(GTK_WINDOW(win), fmon_gui_main_window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *view = gtk_text_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *close = gtk_button_new_with_label(_("关闭"));

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             (ctx != NULL) ? ctx : _("（未找到上下文）"), -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_widget_set_margin_end(close, 8);
    gtk_widget_set_margin_bottom(close, 8);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy),
                             win);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), close);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}

typedef struct {
    LogPage *page;
    gchar   *fallback;
    gchar   *key;
    guint    gen;
} CtxFetch;

static void on_ctx_fetch_done(gpointer user_data, const FmonChildResult *res,
                              gboolean ok)
{
    CtxFetch *c = user_data;

    if (fmon_gui_main_window() == NULL) {
        g_object_unref(c->page->root);
        g_free(c->fallback);
        g_free(c->key);
        g_free(c);
        return;
    }

    if (ok && c->gen == c->page->generation && res != NULL &&
        res->out != NULL && res->out[0] != '\0') {
        if (c->page->ctx_cache == NULL)
            c->page->ctx_cache = g_hash_table_new_full(
                g_str_hash, g_str_equal, g_free, g_free);

        if (g_hash_table_size(c->page->ctx_cache) > 200)
            g_hash_table_remove_all(c->page->ctx_cache);

        g_hash_table_replace(c->page->ctx_cache, g_strdup(c->key),
                             g_strdup(res->out));
        show_context_window(res->out);
    } else {
        show_context_window(c->fallback);
    }

    g_object_unref(c->page->root);
    g_free(c->fallback);
    g_free(c->key);
    g_free(c);
}

static void fetch_file_context(LogPage *page, const char *line, guint occ,
                               const char *fallback)
{
    CtxFetch *c = g_new0(CtxFetch, 1);

    c->page = page;
    c->fallback = g_strdup(fallback);
    c->key = g_strdup_printf("%u\x1f%s", occ, line);
    c->gen = page->generation;

    /* grep 定位第 occ 次出现的行号，awk 输出前后 5 行；参数全部按位置传入 */
    const char *script =
        "n=$(grep -n -F -- \"$1\" \"$2\" 2>/dev/null | sed -n \"$3p\" | cut -d: -f1)\n"
        "[ -n \"$n\" ] || n=$(grep -n -F -m 1 -- \"$1\" \"$2\" 2>/dev/null | cut -d: -f1)\n"
        "[ -n \"$n\" ] || exit 3\n"
        "a=$((n > 5 ? n - 5 : 1))\n"
        "b=$((n + 5))\n"
        "awk -v a=$a -v b=$b 'NR>=a && NR<=b' \"$2\"\n";
    gchar *occ_s = g_strdup_printf("%u", occ);
    const char *argv[] = { "/bin/sh", "-c", script, "sh", line,
                           page->path, occ_s, NULL };

    g_object_ref(page->root);
    fmon_gui_run_async_full(argv, 256 * 1024, 5000, on_ctx_fetch_done, c);
    g_free(occ_s);
}

void log_context_pid_click(GtkGestureClick *gesture, int n_press, double x,
                         double y, gpointer data)
{
    (void)gesture;
    (void)n_press;

    LogPage *page = data;
    GtkTextView *tv = GTK_TEXT_VIEW(page->text_view);
    GtkTextIter iter;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);

    gtk_text_view_get_iter_at_location(tv, &iter, (int)x, (int)y);

    GtkTextTag *tag = gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(buf), "pid-link");
    gboolean tagged = (tag != NULL) && gtk_text_iter_has_tag(&iter, tag);

    if (!tagged && n_press < 2)   /* 普通单击用于选择文本 */
        return;

    GtkTextIter ls, le;
    int line_no = gtk_text_iter_get_line(&iter);

    gtk_text_buffer_get_iter_at_line(buf, &ls, line_no);
    le = ls;
    gtk_text_iter_forward_to_line_end(&le);

    gchar *line = gtk_text_buffer_get_text(buf, &ls, &le, FALSE);
    int off = gtk_text_iter_get_line_offset(&iter);
    int n = (int)strlen(line);

    if (off >= 0 && off < n) {
        int a = off, b = off;

        while (a > 0 && g_ascii_isdigit(line[a - 1]))
            a--;
        while (b < n && g_ascii_isdigit(line[b]))
            b++;

        if (tagged && b > a) {
            gchar *num = g_strndup(line + a, b - a);
            guint32 pid = (guint32)g_ascii_strtoull(num, NULL, 10);

            g_free(num);

            if (pid > 0) {
                fmon_gui_switch_page("proc");
                gui_process_select_pid(pid);
                g_free(line);
                return;
            }
        }
    }

    /* 没点中 PID：弹出该行前后 5 行上下文。
     * line 仍在下面被 log_render_raw_context()/g_strdup() 使用，
     * 不能在这里提前释放。 */
    GtkTextIter a = iter, b = iter;

    for (int i = 0; i < 5; i++) {
        if (!gtk_text_iter_backward_line(&a))
            break;
    }
    for (int i = 0; i < 5; i++) {
        if (!gtk_text_iter_forward_line(&b))
            break;
    }

    gchar *ctx = log_render_raw_context(page, line);

    if (ctx == NULL)
        ctx = gtk_text_buffer_get_text(buf, &a, &b, FALSE);

    if (page->cmd == NULL && page->path != NULL &&
        g_file_test(page->path, G_FILE_TEST_IS_REGULAR)) {
        /* 折叠行带“（重复 N 次）”后缀，定位时去掉后缀再查原文件 */
        gchar *base = g_strdup(line);
        gchar *mark = strstr(base, _("（重复 "));

        if (mark != NULL)
            *mark = '\0';

        g_free(line);
        line = base;

        /* 统计这是当前视图中第几次出现，同名行才能定位到点击的那一处 */
        int clicked_line = gtk_text_iter_get_line(&iter);
        GtkTextIter bs, be;

        gtk_text_buffer_get_start_iter(buf, &bs);
        gtk_text_buffer_get_iter_at_line(buf, &be, clicked_line);
        gtk_text_iter_forward_to_line_end(&be);

        gchar *pre = gtk_text_buffer_get_text(buf, &bs, &be, FALSE);
        gchar **pl = g_strsplit(pre, "\n", -1);
        guint occ = 0;

        for (int i = 0; pl[i] != NULL; i++)
            if (strcmp(pl[i], line) == 0)
                occ++;

        if (occ == 0)
            occ = 1;

        g_strfreev(pl);
        g_free(pre);

        gchar *key = g_strdup_printf("%u\x1f%s", occ, line);
        const gchar *cached = (page->ctx_cache != NULL)
                                  ? g_hash_table_lookup(page->ctx_cache, key)
                                  : NULL;

        if (cached != NULL) {
            show_context_window(cached);
            g_free(key);
            g_free(ctx);
            g_free(line);
            return;
        }

        g_free(key);
        fetch_file_context(page, line, occ, ctx);
        g_free(ctx);
        g_free(line);
        return;
    }

    show_context_window(ctx);
    g_free(ctx);
    g_free(line);
}
