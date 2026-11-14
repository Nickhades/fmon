/*
 * rctl_view.c - 资源限制（rctl）页面
 *
 * 只读列出当前规则（rctl -h，无需 root）。
 * 添加/删除需要 root：优先走控制通道 RCTL ADD/REMOVE。
 *
 * 注意：RACCT/RCTL 需要内核支持，未启用时 rctl 会提示
 * “disabled; enable using kern.racct.enable=1”，页面会显示横幅并禁用按钮。
 */

#include "rctl_view.h"

#include "gui_ctl.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <string.h>

typedef struct {
    GtkWidget *root;
    GtkWidget *list;
    GtkWidget *banner;
    GtkWidget *btn_add;
    GtkWidget *btn_remove;
    gboolean   enabled;
    guint      timer;
} RctlPage;

static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);

    if (id != 0)
        g_source_remove(id);
}

static void set_enabled(RctlPage *page, gboolean enabled, const char *message)
{
    page->enabled = enabled;

    gtk_widget_set_sensitive(page->btn_add, enabled);
    gtk_widget_set_sensitive(page->btn_remove, enabled);

    if (enabled) {
        gtk_widget_set_visible(page->banner, FALSE);
        return;
    }

    gtk_label_set_text(GTK_LABEL(page->banner),
                       (message != NULL) ? message : "");
    gtk_widget_set_visible(page->banner, TRUE);
}

/* 显示“不可用”的原因：0 = 未启用，1 = 无权限，2 = 其他错误 */
static void show_unavailable(RctlPage *page, int kind, const char *detail)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(page->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(page->list), child);

    const char *message;
    const char *hint;

    if (kind == 0) {
        message = "RACCT/RCTL 未启用：需要内核启用 RACCT/RCTL";
        hint = "在 /boot/loader.conf 中加入 kern.racct.enable=1 并重启后可用。";
    } else if (kind == 1) {
        message = "需要 root 权限才能读取 rctl 规则";
        hint = "普通用户无法直接读取：请启动 fmon-daemon（控制通道）后重试，"
               "或以 root 运行 fmon-gui。";
    } else {
        message = "读取 rctl 失败";
        hint = (detail != NULL && detail[0] != '\0') ? detail : "未知原因";
    }

    set_enabled(page, FALSE, message);

    GtkWidget *row = gtk_label_new(hint);
    gtk_widget_add_css_class(row, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(row), TRUE);
    gtk_widget_set_margin_top(row, 16);
    gtk_list_box_append(GTK_LIST_BOX(page->list), row);
}

/* 把规则文本填进列表 */
static void show_rules(RctlPage *page, const char *text)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(page->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(page->list), child);

    set_enabled(page, TRUE, NULL);

    gchar **lines = g_strsplit((text != NULL) ? text : "", "\n", -1);
    int     count = 0;

    for (int i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(lines[i]);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_widget_set_margin_start(label, 6);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "rule", g_strdup(lines[i]),
                               g_free);
        gtk_list_box_append(GTK_LIST_BOX(page->list), row);
        count++;
    }

    g_strfreev(lines);

    if (count == 0) {
        GtkWidget *row = gtk_label_new("当前没有任何 rctl 规则。");
        gtk_widget_add_css_class(row, "dim-label");
        gtk_widget_set_margin_top(row, 16);
        gtk_list_box_append(GTK_LIST_BOX(page->list), row);
    }
}

/* 本地回退（以 root 运行，或系统允许读取时） */
static void on_rctl_list_done(gpointer user_data, const FmonChildResult *res,
                              gboolean ok)
{
    RctlPage *page = user_data;
    const char *out = (res != NULL && res->out != NULL) ? res->out : "";

    if (strstr(out, "disabled") != NULL) {
        show_unavailable(page, 0, out);
        return;
    }

    if (strstr(out, "Operation not permitted") != NULL ||
        strstr(out, "Permission denied") != NULL) {
        show_unavailable(page, 1, out);
        return;
    }

    if (!ok || res == NULL || res->out == NULL) {
        show_unavailable(page, 2, out);
        return;
    }

    show_rules(page, res->out);
}

static void refresh_rules(RctlPage *page)
{
    /* 优先走控制通道：守护进程以 root 执行，普通用户也能看到规则 */
    if (fmon_ctl_available()) {
        gchar         *out = NULL;
        gchar         *err = NULL;
        FmonCtlResult  r   = fmon_ctl_rctl_list(&out, &err);

        if (r == FMON_CTL_OK) {
            show_rules(page, out);
            g_free(out);
            g_free(err);
            return;
        }

        if (r == FMON_CTL_ERROR) {
            if (err != NULL && strstr(err, "disabled") != NULL)
                show_unavailable(page, 0, err);
            else
                show_unavailable(page, 2, err);

            g_free(out);
            g_free(err);
            return;
        }

        g_free(out);
        g_free(err);
    }

    const char *argv[] = { "/usr/bin/rctl", "-h", NULL };

    fmon_gui_run_async_full(argv, 256 * 1024, 4000, on_rctl_list_done, page);
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    refresh_rules(data);
}

/* ---------------------------------------------------------------------------
 * 添加规则
 * ----------------------------------------------------------------------- */
typedef struct {
    RctlPage  *page;
    GtkWidget *win;
    GtkWidget *subject;
    GtkWidget *type;
    GtkWidget *value;
    GtkWidget *status;
} AddDlg;

static void on_add_apply(GtkButton *btn, gpointer data)
{
    (void)btn;

    AddDlg *dlg = data;

    const char *subject = gtk_editable_get_text(GTK_EDITABLE(dlg->subject));
    const char *value   = gtk_editable_get_text(GTK_EDITABLE(dlg->value));

    if (subject == NULL || subject[0] == '\0' || value == NULL ||
        value[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(dlg->status), "subject 和数值不能为空");
        return;
    }

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dlg->type));

    static const char *const types[] = {
        "memoryuse", "pcpu", "maxproc", "openfiles", "cputime"
    };
    if (sel >= G_N_ELEMENTS(types)) {
        gtk_label_set_text(GTK_LABEL(dlg->status), "请选择限制类型");
        return;
    }

    gchar *rule = g_strdup_printf("%s:%s:deny=%s", subject, types[sel], value);

    if (fmon_ctl_available()) {
        gchar         *err = NULL;
        FmonCtlResult  r   = fmon_ctl_rctl(TRUE, rule, &err);

        if (r == FMON_CTL_OK) {
            g_free(rule);
            g_free(err);
            gtk_window_destroy(GTK_WINDOW(dlg->win));
            refresh_rules(dlg->page);
            return;
        }
        /* 控制通道不可用：直接报错，不再弹密码框 */
        g_free(err);
    }

    gtk_label_set_text(GTK_LABEL(dlg->status),
                       "控制通道不可用：请确认 fmon-daemon 正在运行");
    g_free(rule);
}

static void on_add_close(GtkButton *btn, gpointer data)
{
    (void)btn;
    gtk_window_destroy(GTK_WINDOW(data));
}

static void on_add_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    RctlPage *page = data;
    AddDlg   *dlg  = g_new0(AddDlg, 1);

    dlg->page = page;
    dlg->win  = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg->win), "添加资源限制");
    gtk_window_set_default_size(GTK_WINDOW(dlg->win), 460, -1);
    gtk_window_set_transient_for(GTK_WINDOW(dlg->win),
                                 GTK_WINDOW(gtk_widget_get_root(page->root)));
    gtk_window_set_modal(GTK_WINDOW(dlg->win), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(vbox, 18);
    gtk_widget_set_margin_bottom(vbox, 18);
    gtk_widget_set_margin_start(vbox, 18);
    gtk_widget_set_margin_end(vbox, 18);

    GtkWidget *l1 = gtk_label_new("对象（例如 process:1234 或 user:zxy）");
    gtk_widget_set_halign(l1, GTK_ALIGN_START);
    dlg->subject = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dlg->subject), "process:1234");
    gtk_box_append(GTK_BOX(vbox), l1);
    gtk_box_append(GTK_BOX(vbox), dlg->subject);

    GtkWidget *l2 = gtk_label_new("限制类型");
    gtk_widget_set_halign(l2, GTK_ALIGN_START);
    const char *types[] = { "memoryuse", "pcpu", "maxproc", "openfiles",
                            "cputime", NULL };
    dlg->type = gtk_drop_down_new_from_strings(types);
    gtk_box_append(GTK_BOX(vbox), l2);
    gtk_box_append(GTK_BOX(vbox), dlg->type);

    GtkWidget *l3 = gtk_label_new("数值（memoryuse 可用 K/M/G 后缀）");
    gtk_widget_set_halign(l3, GTK_ALIGN_START);
    dlg->value = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dlg->value), "2G");
    gtk_box_append(GTK_BOX(vbox), l3);
    gtk_box_append(GTK_BOX(vbox), dlg->value);

    dlg->status = gtk_label_new("");
    gtk_widget_add_css_class(dlg->status, "small-text");
    gtk_widget_set_halign(dlg->status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), dlg->status);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("取消");
    GtkWidget *apply  = gtk_button_new_with_label("添加");
    gtk_widget_add_css_class(apply, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_add_close), dlg->win);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_add_apply), dlg);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), apply);
    gtk_box_append(GTK_BOX(vbox), actions);

    gtk_window_set_child(GTK_WINDOW(dlg->win), vbox);
    g_object_set_data_full(G_OBJECT(dlg->win), "add-dlg", dlg, g_free);
    gtk_window_present(GTK_WINDOW(dlg->win));
}

/* ---------------------------------------------------------------------------
 * 删除规则
 * ----------------------------------------------------------------------- */
static void on_remove_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    RctlPage *page = data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(page->list));

    if (row == NULL) {
        gtk_label_set_text(GTK_LABEL(page->banner), "请先选中要删除的规则");
        gtk_widget_set_visible(page->banner, TRUE);
        return;
    }

    const char *rule = g_object_get_data(G_OBJECT(row), "rule");

    if (rule == NULL)
        return;

    if (fmon_ctl_available()) {
        gchar         *err = NULL;
        FmonCtlResult  r   = fmon_ctl_rctl(FALSE, rule, &err);

        if (r == FMON_CTL_OK) {
            g_free(err);
            refresh_rules(page);
            return;
        }
        /* 控制通道不可用：直接报错，不再弹密码框 */
        g_free(err);
    }

    gtk_label_set_text(GTK_LABEL(page->banner),
                       "控制通道不可用：请确认 fmon-daemon 正在运行");
    gtk_widget_set_visible(page->banner, TRUE);
}

static gboolean on_tick(gpointer data)
{
    RctlPage *page = data;

    if (page->root == NULL || !gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    refresh_rules(page);
    return G_SOURCE_CONTINUE;
}

GtkWidget *gui_rctl_view_new(void)
{
    RctlPage *page = g_new0(RctlPage, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(vbox, TRUE);
    gtk_widget_add_css_class(vbox, "view-card");
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_refresh = gtk_button_new_with_label("刷新");
    gtk_widget_add_css_class(btn_refresh, "suggested-action");

    page->btn_add    = gtk_button_new_with_label("添加限制");
    page->btn_remove = gtk_button_new_with_label("删除选中");

    gtk_box_append(GTK_BOX(toolbar), btn_refresh);
    gtk_box_append(GTK_BOX(toolbar), page->btn_add);
    gtk_box_append(GTK_BOX(toolbar), page->btn_remove);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    page->banner = gtk_label_new("");
    gtk_widget_add_css_class(page->banner, "small-text");
    gtk_label_set_wrap(GTK_LABEL(page->banner), TRUE);
    gtk_widget_set_halign(page->banner, GTK_ALIGN_START);
    gtk_widget_set_visible(page->banner, FALSE);
    gtk_box_append(GTK_BOX(vbox), page->banner);

    page->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list),
                                    GTK_SELECTION_SINGLE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                     page);
    g_signal_connect(page->btn_add, "clicked", G_CALLBACK(on_add_clicked),
                     page);
    g_signal_connect(page->btn_remove, "clicked",
                     G_CALLBACK(on_remove_clicked), page);

    set_enabled(page, FALSE, "正在检测 RACCT/RCTL…");
    refresh_rules(page);

    page->timer = g_timeout_add_seconds(10, on_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "rctl-timer",
                           GUINT_TO_POINTER(page->timer), remove_source);
    g_object_set_data_full(G_OBJECT(vbox), "rctl-page", page, g_free);

    return vbox;
}
