/*
 * process_windows.c - 进程页的查看类窗口
 *
 * 进程详情（ps -p）与 lsof / sockstat 文本窗口；只读取数据，
 * 不发送信号、不修改进程状态。
 */

#include <glib/gi18n.h>

#include "process_internal.h"

/* 进程详情窗口：异步 ps 结果回来之前窗口可能已被关闭，用弱指针判断。 */
typedef struct {
    GtkWindow *window;   /* 弱指针 */
    guint32    pid;
    gchar     *name;
} ProcDetailCtx;

/* lsof / sockstat 文本窗口 */
typedef struct {
    GtkWindow *window;   /* 弱指针 */
    gchar     *title;
    guint32    pid_filter;   /* 非 0 时只显示该 pid 的 sockstat 行 */
} TextViewCtx;

/* ===========================================================================
 * 进程详情窗口
 * ========================================================================= */
static void proc_detail_free(ProcDetailCtx *ctx)
{
    if (ctx == NULL)
        return;
    g_free(ctx->name);
    g_free(ctx);
}

static void on_detail_done(gpointer user_data, const FmonChildResult *res,
                           gboolean ok)
{
    ProcDetailCtx *ctx = user_data;

    if (ctx->window == NULL) {
        proc_detail_free(ctx);
        return;
    }

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_margin_top(grid, 18);
    gtk_widget_set_margin_bottom(grid, 18);
    gtk_widget_set_margin_start(grid, 18);
    gtk_widget_set_margin_end(grid, 18);

    int row = 0;

    #define ADD_DETAIL_ROW(key, value)                                        \
        do {                                                                  \
            GtkWidget *k = gtk_label_new(key);                                 \
            gtk_widget_add_css_class(k, "dim-label");                          \
            gtk_widget_set_halign(k, GTK_ALIGN_END);                           \
            gtk_widget_set_valign(k, GTK_ALIGN_START);                         \
            GtkWidget *v = gtk_label_new((value) != NULL ? (value) : "");      \
            gtk_label_set_wrap(GTK_LABEL(v), TRUE);                            \
            gtk_label_set_selectable(GTK_LABEL(v), TRUE);                      \
            gtk_label_set_xalign(GTK_LABEL(v), 0.0f);                          \
            gtk_widget_set_halign(v, GTK_ALIGN_START);                         \
            gtk_widget_set_hexpand(v, TRUE);                                   \
            gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);                  \
            gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);                  \
            row++;                                                             \
        } while (0)

    gchar pid_buf[16];
    g_snprintf(pid_buf, sizeof(pid_buf), "%u", ctx->pid);
    ADD_DETAIL_ROW("PID", pid_buf);
    ADD_DETAIL_ROW("名称", (ctx->name != NULL) ? ctx->name : "");

    if (ok && res != NULL && res->out != NULL && res->out[0] != '\0') {
        gchar **lines = g_strsplit(res->out, "\n", -1);

        /* 第二行是数据行（第一行是 ps 的表头） */
        if (lines[0] != NULL && lines[1] != NULL && lines[1][0] != '\0')
            ADD_DETAIL_ROW("完整信息", lines[1]);
        else
            ADD_DETAIL_ROW("完整信息", lines[0]);

        g_strfreev(lines);
    } else {
        ADD_DETAIL_ROW("完整信息", "无法获取（进程可能已退出）");
    }

    #undef ADD_DETAIL_ROW

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
    gtk_window_set_child(ctx->window, scroll);
    gtk_window_set_title(ctx->window, _("进程详细信息"));

    proc_detail_free(ctx);
}

void proc_windows_show_detail(GtkWindow *parent, ProcItem *item)
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), _("进程详细信息（加载中…）"));
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 420);
    if (parent != NULL)
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    GtkWidget *loading = gtk_label_new(_("正在读取进程信息…"));
    gtk_window_set_child(GTK_WINDOW(win), loading);
    gtk_window_present(GTK_WINDOW(win));

    ProcDetailCtx *ctx = g_new0(ProcDetailCtx, 1);
    ctx->window = GTK_WINDOW(win);
    ctx->pid    = item->pid;
    ctx->name   = g_strdup(item->comm);
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&ctx->window);

    gchar *pid_str = g_strdup_printf("%u", item->pid);
    const char *argv[] = {
        "env", "LC_ALL=C", "ps", "-ww", "-p", pid_str, "-o",
        "pid,ppid,user,group,pri,nice,stat,time,vsz,rss,wchan,command", NULL
    };

    fmon_gui_run_async_full(argv, 256 * 1024, 4000, on_detail_done, ctx);
    g_free(pid_str);
}

/* ===========================================================================
 * 文本输出窗口（lsof / sockstat）
 * ========================================================================= */
static void on_textview_done(gpointer user_data, const FmonChildResult *res,
                             gboolean ok)
{
    TextViewCtx *ctx = user_data;

    if (ctx->window == NULL) {
        g_free(ctx->title);
        g_free(ctx);
        return;
    }

    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_window_set_child(ctx->window, scroll);

    gchar *filtered = NULL;
    const gchar *text = NULL;

    if (ok && res != NULL && res->out != NULL && res->out[0] != '\0') {
        if (ctx->pid_filter != 0) {
            /* sockstat 第 3 列是 PID：这里按 pid 过滤，不需要 shell + awk */
            GString *out = g_string_new(NULL);
            gchar  **lines = g_strsplit(res->out, "\n", -1);

            for (int i = 0; lines[i] != NULL; i++) {
                gchar **tok = g_strsplit_set(lines[i], " \t", -1);
                if (i == 0) {          /* 保留表头 */
                    g_string_append(out, lines[i]);
                    g_string_append_c(out, '\n');
                    g_strfreev(tok);
                    continue;
                }
                guint idx = 0;
                for (int j = 0; tok[j] != NULL; j++) {
                    if (tok[j][0] == '\0')
                        continue;
                    idx++;
                    if (idx == 3) {
                        if ((guint32)g_ascii_strtoull(tok[j], NULL, 10) ==
                            ctx->pid_filter) {
                            g_string_append(out, lines[i]);
                            g_string_append_c(out, '\n');
                        }
                        break;
                    }
                }
                g_strfreev(tok);
            }

            g_strfreev(lines);

            if (out->len == 0)
                g_string_append(out, "该进程当前没有网络连接。\n");

            filtered = g_string_free(out, FALSE);
            text = filtered;
        } else {
            text = res->out;
        }
    } else {
        text = "没有可显示的内容（进程可能已退出，或未安装相应工具）。";
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gchar *valid = fmon_gui_make_valid_utf8(text, -1);
    gtk_text_buffer_set_text(buffer, valid, -1);
    g_free(valid);
    g_free(filtered);

    g_free(ctx->title);
    g_free(ctx);
}

void proc_windows_show_text(GtkWindow *parent, const char *title,
                             guint32 pid_filter, const char *const argv[])
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 520);
    if (parent != NULL)
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);

    GtkWidget *loading = gtk_label_new(_("正在读取…"));
    gtk_window_set_child(GTK_WINDOW(win), loading);
    gtk_window_present(GTK_WINDOW(win));

    TextViewCtx *ctx = g_new0(TextViewCtx, 1);
    ctx->window     = GTK_WINDOW(win);
    ctx->title      = g_strdup(title);
    ctx->pid_filter = pid_filter;
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&ctx->window);

    fmon_gui_run_async_full(argv, 4 * 1024 * 1024, 8000, on_textview_done, ctx);
}
