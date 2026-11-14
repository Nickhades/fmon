/*
 * alert_view.c - 告警历史页面
 *
 * 读取 ~/.local/state/fmon/alerts.log（alerts.c 写入），按时间倒序展示
 * 最近 300 条告警。支持静默 1 小时（只影响桌面通知，不影响记录）和清空历史。
 */

#include "alert_view.h"

#include "alerts.h"

#include <glib/gstdio.h>
#include <string.h>

#define ALERT_VIEW_MAX_ROWS 300

typedef struct {
    GtkWidget *root;
    GtkWidget *list;
    GtkWidget *silence_btn;
    GtkWidget *status;
    guint      timer;
} AlertPage;

static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);

    if (id != 0)
        g_source_remove(id);
}

/* 解析一行：时间 \t 级别 \t 指标 \t 值 \t 阈值 \t 消息 */
static gboolean parse_line(const char *line, char **time_out,
                           gboolean *critical_out, char **msg_out)
{
    char **parts = g_strsplit(line, "\t", 6);

    if (g_strv_length(parts) < 6) {
        g_strfreev(parts);
        return FALSE;
    }

    *time_out     = g_strdup(parts[0]);
    *critical_out = (strcmp(parts[1], "CRITICAL") == 0);
    *msg_out      = g_strdup(parts[5]);

    g_strfreev(parts);
    return TRUE;
}

static void rebuild_list(AlertPage *page)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(page->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(page->list), child);

    const gchar *path = alerts_log_path();
    gchar       *content = NULL;

    if (!g_file_get_contents(path, &content, NULL, NULL) ||
        content[0] == '\0') {
        g_free(content);

        GtkWidget *empty = gtk_label_new(
            "还没有告警记录。超过阈值的事件会自动记录在这里。");
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_widget_set_margin_top(empty, 24);
        gtk_widget_set_margin_bottom(empty, 24);
        gtk_list_box_append(GTK_LIST_BOX(page->list), empty);

        if (page->status != NULL)
            gtk_label_set_text(GTK_LABEL(page->status), "暂无告警记录");
        return;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);

    int total = (int)g_strv_length(lines);
    int shown = 0;

    /* 从文件末尾往前取，最新的在最上面 */
    for (int i = total - 1; i >= 0 && shown < ALERT_VIEW_MAX_ROWS; i--) {
        if (lines[i][0] == '\0')
            continue;

        char    *ts       = NULL;
        char    *msg      = NULL;
        gboolean critical = FALSE;

        if (!parse_line(lines[i], &ts, &critical, &msg))
            continue;

        GtkWidget *row  = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(hbox, "alert-row");

        GtkWidget *badge = gtk_label_new(critical ? "严重" : "警告");
        gtk_widget_add_css_class(badge,
                                 critical ? "alert-badge-critical"
                                          : "alert-badge-warning");
        gtk_widget_set_valign(badge, GTK_ALIGN_START);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(vbox, TRUE);

        GtkWidget *msg_label = gtk_label_new(msg);
        gtk_label_set_xalign(GTK_LABEL(msg_label), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(msg_label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(msg_label), PANGO_WRAP_WORD_CHAR);

        GtkWidget *time_label = gtk_label_new(ts);
        gtk_widget_add_css_class(time_label, "alert-time");
        gtk_label_set_xalign(GTK_LABEL(time_label), 0.0f);

        gtk_box_append(GTK_BOX(vbox), msg_label);
        gtk_box_append(GTK_BOX(vbox), time_label);
        gtk_box_append(GTK_BOX(hbox), badge);
        gtk_box_append(GTK_BOX(hbox), vbox);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
        gtk_list_box_append(GTK_LIST_BOX(page->list), row);

        g_free(ts);
        g_free(msg);
        shown++;
    }

    g_strfreev(lines);

    if (page->status != NULL) {
        gchar *status = g_strdup_printf("显示最近 %d 条 · %s", shown,
                                        alerts_log_path());
        gtk_label_set_text(GTK_LABEL(page->status), status);
        g_free(status);
    }

    if (shown == 0) {
        GtkWidget *empty = gtk_label_new("还没有告警记录。");
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_list_box_append(GTK_LIST_BOX(page->list), empty);
    }
}

static void update_silence_button(AlertPage *page)
{
    if (alerts_is_silenced()) {
        gchar *text = g_strdup_printf("静默中（剩 %d 分钟）",
                                      alerts_silenced_minutes_left());
        gtk_button_set_label(GTK_BUTTON(page->silence_btn), text);
        g_free(text);
        gtk_widget_add_css_class(page->silence_btn, "suggested-action");
    } else {
        gtk_button_set_label(GTK_BUTTON(page->silence_btn), "静默 1 小时");
        gtk_widget_remove_css_class(page->silence_btn, "suggested-action");
    }
}

static void on_silence_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    AlertPage *page = data;

    alerts_silence(alerts_is_silenced() ? 0 : 60);
    update_silence_button(page);
}

static void on_clear_confirmed(GObject *source, GAsyncResult *res,
                               gpointer data)
{
    AlertPage      *page = data;
    GtkAlertDialog *dlg  = GTK_ALERT_DIALOG(source);

    if (gtk_alert_dialog_choose_finish(dlg, res, NULL) != 1)
        return;   /* 0 = 取消，1 = 清空 */

    g_remove(alerts_log_path());
    rebuild_list(page);
    gtk_label_set_text(GTK_LABEL(page->status), "历史已清空");
}

static void on_clear_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    AlertPage *page = data;
    GtkAlertDialog *dlg = gtk_alert_dialog_new("清空全部告警历史？");
    const char *buttons[] = { "取消", "清空", NULL };

    gtk_alert_dialog_set_detail(dlg, "该操作不可撤销。");
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(gtk_widget_get_root(page->root)),
                            NULL, on_clear_confirmed, page);
    g_object_unref(dlg);
}

static gboolean on_tick(gpointer data)
{
    AlertPage *page = data;

    if (page->root == NULL || !gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    rebuild_list(page);
    update_silence_button(page);

    return G_SOURCE_CONTINUE;
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    rebuild_list(data);
}

GtkWidget *gui_alert_view_new(void)
{
    AlertPage *page = g_new0(AlertPage, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_add_css_class(vbox, "view-card");
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_refresh = gtk_button_new_with_label("刷新");
    gtk_widget_add_css_class(btn_refresh, "suggested-action");

    page->silence_btn = gtk_button_new_with_label("静默 1 小时");
    gtk_widget_set_tooltip_text(page->silence_btn,
                                "静默期间只记录历史，不弹桌面通知");

    GtkWidget *btn_clear = gtk_button_new_with_label("清空历史");
    gtk_widget_set_tooltip_text(btn_clear, "删除 ~/.local/state/fmon/alerts.log");

    gtk_box_append(GTK_BOX(toolbar), btn_refresh);
    gtk_box_append(GTK_BOX(toolbar), page->silence_btn);
    gtk_box_append(GTK_BOX(toolbar), btn_clear);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    page->status = gtk_label_new("");
    gtk_widget_add_css_class(page->status, "small-text");
    gtk_widget_set_halign(page->status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), page->status);

    page->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(page->list, "alert-list");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     page);
    g_signal_connect(page->silence_btn, "clicked",
                     G_CALLBACK(on_silence_clicked), page);
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_clear_clicked), page);

    rebuild_list(page);
    update_silence_button(page);

    page->timer = g_timeout_add_seconds(5, on_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "alert-timer",
                           GUINT_TO_POINTER(page->timer), remove_source);
    g_object_set_data_full(G_OBJECT(vbox), "alert-page", page, g_free);

    return vbox;
}
