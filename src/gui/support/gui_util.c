/*
 * gui_util.c - 界面层公共工具实现
 */

#include "gui_util.h"

#include "theme.h"

#include <stdint.h>
#include <string.h>

/* 由 main.c 提供 */
extern GtkWindow *g_main_window;

/* ---------------------------------------------------------------------------
 * 图表面板背景
 * ------------------------------------------------------------------------- */

static void panel_path(cairo_t *cr, double x, double y, double w, double h,
                       double r)
{
    const double q = 1.5707963267948966;

    if (r > w / 2.0)
        r = w / 2.0;
    if (r > h / 2.0)
        r = h / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -q, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, q);
    cairo_arc(cr, x + r, y + h - r, r, q, 2.0 * q);
    cairo_arc(cr, x + r, y + r, r, 2.0 * q, 3.0 * q);
    cairo_close_path(cr);
}

void fmon_gui_draw_panel(cairo_t *cr, double x, double y, double w, double h,
                         double border_r, double border_g, double border_b,
                         double border_a)
{
    if (w <= 2.0 || h <= 2.0)
        return;

    panel_path(cr, x, y, w, h, 10.0);

    double top[4];
    double bottom[4];

    fmon_theme_panel_colors(top, bottom);

    cairo_pattern_t *grad = cairo_pattern_create_linear(0.0, y, 0.0, y + h);

    cairo_pattern_add_color_stop_rgba(grad, 0.0, top[0], top[1], top[2],
                                      top[3]);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, bottom[0], bottom[1],
                                      bottom[2], bottom[3]);

    cairo_set_source(cr, grad);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(grad);

    /* 几何纹理：横竖网格（小方块），裁剪在圆角面板内，只作背景质感 */
    cairo_save(cr);
    panel_path(cr, x, y, w, h, 10.0);
    cairo_clip(cr);
    cairo_set_line_width(cr, 1.0);

    double tex[4];

    fmon_theme_texture_color(tex);
    cairo_set_source_rgba(cr, tex[0], tex[1], tex[2], tex[3]);

    for (double gx = x + 16.0; gx < x + w; gx += 16.0) {
        cairo_move_to(cr, gx, y);
        cairo_line_to(cr, gx, y + h);
        cairo_stroke(cr);
    }

    for (double gy = y + 16.0; gy < y + h; gy += 16.0) {
        cairo_move_to(cr, x, gy);
        cairo_line_to(cr, x + w, gy);
        cairo_stroke(cr);
    }

    cairo_restore(cr);

    if (border_a > 0.0) {
        cairo_set_source_rgba(cr, border_r, border_g, border_b, border_a);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
    }
}

/* ---------------------------------------------------------------------------
 * 异步命令执行
 * ------------------------------------------------------------------------- */
typedef struct {
    gchar          **argv;
    FmonCmdCallback  cb;
    gpointer         user_data;
    size_t           max_bytes;
    int              timeout_ms;
    gboolean         merge_stderr;
} CmdJob;

static void cmd_job_free(CmdJob *job)
{
    if (job == NULL)
        return;
    g_strfreev(job->argv);
    g_free(job);
}

static void cmd_result_free(FmonChildResult *res)
{
    if (res == NULL)
        return;
    fmon_child_result_free(res);
    g_free(res);
}

static void cmd_thread(GTask *task, gpointer source_object, gpointer task_data,
                       GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;

    CmdJob *job = task_data;
    FmonChildResult *res = g_new0(FmonChildResult, 1);

    if (job->merge_stderr)
        (void)fmon_child_run_merged((const char *const *)job->argv,
                                    job->max_bytes, job->timeout_ms, res);
    else
        (void)fmon_child_run((const char *const *)job->argv, job->max_bytes,
                             job->timeout_ms, res);

    g_task_return_pointer(task, res, (GDestroyNotify)cmd_result_free);
}

static void cmd_done(GObject *source_object, GAsyncResult *result,
                     gpointer user_data)
{
    (void)user_data;

    GTask *task = G_TASK(result);
    CmdJob *job = g_task_get_task_data(task);

    FmonChildResult *res = g_task_propagate_pointer(task, NULL);
    gboolean ok = (res != NULL && fmon_child_ok(res));

    if (job->cb != NULL)
        job->cb(job->user_data, res, ok);

    if (res != NULL) {
        fmon_child_result_free(res);
        g_free(res);
    }
}

static void run_async_common(const char *const argv[], size_t max_bytes,
                             int timeout_ms, gboolean merge_stderr,
                             FmonCmdCallback cb, gpointer user_data)
{
    if (argv == NULL || argv[0] == NULL) {
        if (cb != NULL) {
            FmonChildResult empty;
            memset(&empty, 0, sizeof(empty));
            empty.status = -1;
            cb(user_data, &empty, FALSE);
        }
        return;
    }

    CmdJob *job = g_new0(CmdJob, 1);
    job->argv       = g_strdupv((gchar **)argv);
    job->cb         = cb;
    job->user_data  = user_data;
    job->max_bytes    = max_bytes;
    job->timeout_ms   = timeout_ms;
    job->merge_stderr = merge_stderr;

    GTask *task = g_task_new(NULL, NULL, cmd_done, NULL);
    g_task_set_task_data(task, job, (GDestroyNotify)cmd_job_free);
    g_task_run_in_thread(task, cmd_thread);
    g_object_unref(task);
}

void fmon_gui_run_async_full(const char *const argv[], size_t max_bytes,
                             int timeout_ms, FmonCmdCallback cb,
                             gpointer user_data)
{
    run_async_common(argv, max_bytes, timeout_ms, FALSE, cb, user_data);
}

void fmon_gui_run_async_merged(const char *const argv[], FmonCmdCallback cb,
                               gpointer user_data)
{
    run_async_common(argv, 0, 0, TRUE, cb, user_data);
}

void fmon_gui_run_async(const char *const argv[], FmonCmdCallback cb,
                        gpointer user_data)
{
    run_async_common(argv, 0, 0, FALSE, cb, user_data);
}

/* ---------------------------------------------------------------------------
 * 对话框
 * ------------------------------------------------------------------------- */
GtkWindow *fmon_gui_main_window(void)
{
    return g_main_window;
}

void fmon_gui_show_info(GtkWindow *parent, const char *message,
                        const char *detail)
{
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", (message != NULL) ? message : "");

    if (detail != NULL)
        gtk_alert_dialog_set_detail(dlg, detail);
    gtk_alert_dialog_set_buttons(dlg, (const char *[]){ "确定", NULL });
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
}

void fmon_gui_show_error(GtkWindow *parent, const char *message,
                         const char *detail)
{
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", (message != NULL) ? message : "");

    if (detail != NULL)
        gtk_alert_dialog_set_detail(dlg, detail);
    gtk_alert_dialog_set_buttons(dlg, (const char *[]){ "关闭", NULL });
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
}

typedef struct {
    FmonConfirmCallback cb;
    gpointer            user_data;
    GDestroyNotify      user_data_free;
} ConfirmCtx;

static void confirm_done(GObject *source_object, GAsyncResult *result,
                         gpointer user_data)
{
    ConfirmCtx     *ctx = user_data;
    GtkAlertDialog *dlg = GTK_ALERT_DIALOG(source_object);
    GError         *error = NULL;

    int choice = gtk_alert_dialog_choose_finish(dlg, result, &error);

    if (error != NULL)
        g_error_free(error);

    if (choice == 0 && ctx->cb != NULL)
        ctx->cb(ctx->user_data);

    if (ctx->user_data_free != NULL)
        ctx->user_data_free(ctx->user_data);
    g_free(ctx);

    /* 与创建时 g_object_ref 对应 */
    g_object_unref(dlg);
}

void fmon_gui_confirm(GtkWindow *parent, const char *message, const char *detail,
                      const char *accept_label, FmonConfirmCallback on_accept,
                      gpointer user_data, GDestroyNotify user_data_free)
{
    /* 使用 gtk_alert_dialog_new("%s", ...) 而不是把外部文本当格式串，
     * 避免进程名里出现 %s/%n 时触发格式化漏洞。 */
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", (message != NULL) ? message : "");

    if (detail != NULL)
        gtk_alert_dialog_set_detail(dlg, detail);

    const char *buttons[] = { (accept_label != NULL) ? accept_label : "确定",
                              "取消", NULL };
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_default_button(dlg, 1);

    ConfirmCtx *ctx = g_new0(ConfirmCtx, 1);
    ctx->cb             = on_accept;
    ctx->user_data      = user_data;
    ctx->user_data_free = user_data_free;

    g_object_ref(dlg);
    gtk_alert_dialog_choose(dlg, parent, NULL, confirm_done, ctx);
    g_object_unref(dlg);
}

/* ---------------------------------------------------------------------------
 * 格式化
 * ------------------------------------------------------------------------- */
char *fmon_gui_format_bytes(uint64_t bytes)
{
    const double kb = 1024.0;
    double v = (double)bytes;

    if (v >= kb * kb * kb * kb)
        return g_strdup_printf("%.2f TB", v / (kb * kb * kb * kb));
    if (v >= kb * kb * kb)
        return g_strdup_printf("%.2f GB", v / (kb * kb * kb));
    if (v >= kb * kb)
        return g_strdup_printf("%.1f MB", v / (kb * kb));
    if (v >= kb)
        return g_strdup_printf("%.1f KB", v / kb);
    return g_strdup_printf("%.0f B", v);
}

char *fmon_gui_format_rate(double kbps)
{
    if (kbps >= 1024.0 * 1024.0)
        return g_strdup_printf("%.2f GB/s", kbps / (1024.0 * 1024.0));
    if (kbps >= 1024.0)
        return g_strdup_printf("%.2f MB/s", kbps / 1024.0);
    if (kbps >= 1.0)
        return g_strdup_printf("%.1f KB/s", kbps);
    return g_strdup_printf("%.0f B/s", kbps * 1024.0);
}

char *fmon_gui_format_uptime(uint64_t seconds)
{
    uint64_t days    = seconds / 86400;
    uint64_t hours   = (seconds % 86400) / 3600;
    uint64_t minutes = (seconds % 3600) / 60;

    if (days > 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " 天 %" G_GUINT64_FORMAT
                               " 小时 %" G_GUINT64_FORMAT " 分",
                               days, hours, minutes);
    if (hours > 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " 小时 %" G_GUINT64_FORMAT
                               " 分",
                               hours, minutes);
    return g_strdup_printf("%" G_GUINT64_FORMAT " 分", minutes);
}

gboolean fmon_gui_is_valid_token(const gchar *s)
{
    if (s == NULL || s[0] == '\0')
        return FALSE;
    if (!g_ascii_isalnum(s[0]))
        return FALSE;

    for (const gchar *p = s; *p != '\0'; p++) {
        if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '+' ||
              *p == '-'))
            return FALSE;
    }
    return TRUE;
}

gchar *fmon_gui_markup_escape(const gchar *text)
{
    return g_markup_escape_text((text != NULL) ? text : "", -1);
}

gchar *fmon_gui_make_valid_utf8(const gchar *data, gssize len)
{
    if (data == NULL)
        return g_strdup("");

    if (len < 0)
        len = (gssize)strlen(data);

    /* FreeBSD 14+ 的 syslogd 默认写 NUL 分隔的二进制记录，
     * 先把 NUL 换成换行，再交给 g_utf8_make_valid 清洗非法字节。 */
    GString *buf = g_string_sized_new((gsize)len + 1);
    for (gssize i = 0; i < len; i++)
        g_string_append_c(buf, (data[i] == '\0') ? '\n' : data[i]);

    gchar *valid = g_utf8_make_valid(buf->str, (gssize)buf->len);
    g_string_free(buf, TRUE);
    return valid;
}
