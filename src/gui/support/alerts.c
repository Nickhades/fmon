/*
 * alerts.c - 阈值告警与桌面通知实现
 */

#include <glib/gi18n.h>

#include "alerts.h"

#include "gui_util.h"
#include "mem_summary.h"

#include <glib/gstdio.h>

#include <string.h>

#define ALERT_COOLDOWN_MS  (10 * 60 * 1000)   /* 同一指标 10 分钟内只通知一次 */

typedef enum {
    ALERT_CPU = 0,
    ALERT_MEM,
    ALERT_SWAP,
    ALERT_POOL,
    ALERT_TEMP,
    ALERT_BATTERY,
    ALERT_COUNT
} AlertKind;

static int64_t  g_last_fired[ALERT_COUNT];
static gboolean g_notify_available = FALSE;
static gboolean g_notify_checked   = FALSE;

/* ===========================================================================
 * 设置读写
 * =========================================================================== */
static gchar *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fmon", "alerts.ini", NULL);
}

static void set_defaults(AlertSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->enabled     = TRUE;
    s->cpu_pct     = 90.0;
    s->mem_pct     = 90.0;
    s->swap_pct    = 80.0;
    s->pool_pct    = 90.0;
    s->temp_c      = 90.0;
    s->battery_pct = 15.0;
}

void alerts_load(AlertSettings *settings)
{
    set_defaults(settings);

    gchar    *path = settings_path();
    GKeyFile *kf   = g_key_file_new();

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        settings->enabled =
            g_key_file_get_boolean(kf, "alerts", "enabled", NULL);

        double cpu  = g_key_file_get_double(kf, "alerts", "cpu", NULL);
        double mem  = g_key_file_get_double(kf, "alerts", "mem", NULL);
        double swap = g_key_file_get_double(kf, "alerts", "swap", NULL);
        double pool = g_key_file_get_double(kf, "alerts", "pool", NULL);
        double temp = g_key_file_get_double(kf, "alerts", "temp", NULL);
        double batt = g_key_file_get_double(kf, "alerts", "battery", NULL);

        /* 文件里缺项时 get_double 返回 0，保留默认值 */
        if (cpu > 0.0)
            settings->cpu_pct = cpu;
        if (mem > 0.0)
            settings->mem_pct = mem;
        if (swap > 0.0)
            settings->swap_pct = swap;
        if (pool > 0.0)
            settings->pool_pct = pool;
        if (temp > 0.0)
            settings->temp_c = temp;
        if (batt > 0.0)
            settings->battery_pct = batt;
    }

    g_key_file_free(kf);
    g_free(path);
}

void alerts_save(const AlertSettings *settings)
{
    gchar *path = settings_path();
    gchar *dir  = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);

    GKeyFile *kf = g_key_file_new();

    g_key_file_set_boolean(kf, "alerts", "enabled", settings->enabled);
    g_key_file_set_double(kf, "alerts", "cpu", settings->cpu_pct);
    g_key_file_set_double(kf, "alerts", "mem", settings->mem_pct);
    g_key_file_set_double(kf, "alerts", "swap", settings->swap_pct);
    g_key_file_set_double(kf, "alerts", "pool", settings->pool_pct);
    g_key_file_set_double(kf, "alerts", "temp", settings->temp_c);
    g_key_file_set_double(kf, "alerts", "battery", settings->battery_pct);

    (void)g_key_file_save_to_file(kf, path, NULL);

    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* ===========================================================================
 * 通知
 * =========================================================================== */
static gboolean notify_available(void)
{
    if (!g_notify_checked) {
        g_notify_available = (g_find_program_in_path("notify-send") != NULL);
        g_notify_checked   = TRUE;
    }
    return g_notify_available;
}

static void on_notify_done(gpointer user_data, const FmonChildResult *res,
                           gboolean ok)
{
    (void)res;
    (void)ok;
    g_free(user_data);
}

static void send_notification(const gchar *title, const gchar *body)
{
    if (!notify_available())
        return;

    const char *argv[] = { "notify-send", "-a", "fmon", "-i",
                           "dialog-warning", title, body, NULL };

    fmon_gui_run_async(argv, on_notify_done, g_strdup(body));
}

void alerts_test_notify(void)
{
    if (!notify_available()) {
        fmon_gui_show_error(
            fmon_gui_main_window(), _("无法发送桌面通知"), _("未找到 notify-send 命令，请安装 libnotify（pkg install libnotify）。"));
        return;
    }
    send_notification("fmon 测试通知",
                      "如果你看到这条消息，说明桌面通知工作正常。");
}

/* ===========================================================================
 * 阈值检查 / 告警记录
 * ========================================================================= */
typedef struct {
    AlertKind  kind;
    AlertLevel level;
    const char *metric;      /* 稳定的短标识，写入日志用 */
    double     value;
    double     threshold;
    gchar     *text;
} AlertHit;

static gint64 g_silent_until = 0;   /* epoch 秒；0 表示未静默 */

void alerts_silence(int minutes)
{
    g_silent_until = (minutes > 0)
                         ? (g_get_real_time() / G_USEC_PER_SEC) +
                               (gint64)minutes * 60
                         : 0;
}

gboolean alerts_is_silenced(void)
{
    return g_silent_until > (g_get_real_time() / G_USEC_PER_SEC);
}

int alerts_silenced_minutes_left(void)
{
    gint64 left = g_silent_until - (g_get_real_time() / G_USEC_PER_SEC);

    return (left > 0) ? (int)((left + 59) / 60) : 0;
}

const gchar *alerts_log_path(void)
{
    static gchar *path = NULL;

    if (path == NULL) {
        gchar *dir = g_build_filename(g_get_user_state_dir(), "fmon", NULL);
        path = g_build_filename(dir, "alerts.log", NULL);
        g_free(dir);
    }
    return path;
}

static void add_hit(AlertHit *hits, int *count, AlertKind kind,
                    AlertLevel level, const char *metric, double value,
                    double threshold, gchar *text)
{
    hits[*count].kind      = kind;
    hits[*count].level     = level;
    hits[*count].metric    = metric;
    hits[*count].value     = value;
    hits[*count].threshold = threshold;
    hits[*count].text      = text;
    (*count)++;
}

/* 高阈值指标：超过阈值 5 个单位视为“严重”；电池是低阈值，反向判断 */
static AlertLevel level_high(double value, double threshold)
{
    return (value >= threshold + 5.0) ? ALERT_LEVEL_CRITICAL
                                      : ALERT_LEVEL_WARNING;
}

static AlertLevel level_low(double value, double threshold)
{
    return (value <= threshold - 5.0) ? ALERT_LEVEL_CRITICAL
                                      : ALERT_LEVEL_WARNING;
}

/* 追加一行到告警历史；文件超过 2 MiB 时轮转一次 */
static void alert_log_append(const AlertHit *hit)
{
    const gchar *path = alerts_log_path();
    gchar       *dir  = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GStatBuf st;

    if (g_stat(path, &st) == 0 && st.st_size > 2 * 1024 * 1024) {
        gchar *old = g_strdup_printf("%s.1", path);

        g_unlink(old);
        (void)g_rename(path, old);
        g_free(old);
    }

    GDateTime *dt = g_date_time_new_now_local();
    gchar     *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");

    gchar *line = g_strdup_printf(
        "%s\t%s\t%s\t%.1f\t%.1f\t%s\n", ts,
        (hit->level == ALERT_LEVEL_CRITICAL) ? "CRITICAL" : "WARNING",
        hit->metric, hit->value, hit->threshold, hit->text);

    FILE *fp = fopen(path, "a");

    if (fp != NULL) {
        (void)fputs(line, fp);
        (void)fclose(fp);
    }

    g_free(line);
    g_free(ts);
    g_date_time_unref(dt);
}

gchar *alerts_check(const FmonShmData *snap, const AlertSettings *s)
{
    if (snap == NULL || s == NULL || !s->enabled)
        return NULL;

    AlertHit hits[ALERT_COUNT];
    int      count = 0;

    if (snap->cpu_usage_pct >= s->cpu_pct) {
        add_hit(hits, &count, ALERT_CPU,
                level_high(snap->cpu_usage_pct, s->cpu_pct), "CPU",
                snap->cpu_usage_pct, s->cpu_pct,
                g_strdup_printf(_("CPU 使用率 %.1f%%（阈值 %.0f%%）"),
                                snap->cpu_usage_pct, s->cpu_pct));
    }

    /* FreeBSD 口径：把可用（free+inactive+cache）与 ZFS ARC 都视为可回收，
     * 否则 ARC 一涨就会把 90% 阈值顶爆（Linux 式 Used/Total 算法）。 */
    double mem_eff = gui_mem_effective_used_pct(
        snap->mem_total_bytes, snap->mem_avail_bytes, snap->zfs_arc_size);

    if (mem_eff >= s->mem_pct) {
        add_hit(hits, &count, ALERT_MEM, level_high(mem_eff, s->mem_pct),
                "MEM", mem_eff, s->mem_pct,
                g_strdup_printf(
                    _("内存使用率 %.1f%%（阈值 %.0f%%，已扣除可回收的 "
                      "ARC/缓存）"),
                    mem_eff, s->mem_pct));
    }

    if (snap->swap_total_kb > 0 && snap->swap_used_pct >= s->swap_pct) {
        add_hit(hits, &count, ALERT_SWAP,
                level_high(snap->swap_used_pct, s->swap_pct), "SWAP",
                snap->swap_used_pct, s->swap_pct,
                g_strdup_printf(_("Swap 使用率 %.1f%%（阈值 %.0f%%）"),
                                snap->swap_used_pct, s->swap_pct));
    }

    for (uint32_t i = 0; i < snap->pool_count && i < FMON_MAX_POOLS; i++) {
        if (snap->pools[i].available && snap->pools[i].util_pct >= s->pool_pct) {
            add_hit(hits, &count, ALERT_POOL,
                    level_high(snap->pools[i].util_pct, s->pool_pct), "POOL",
                    snap->pools[i].util_pct, s->pool_pct,
                    g_strdup_printf(_("ZFS 池 %s 使用率 %.1f%%（阈值 %.0f%%）"),
                                    snap->pools[i].name,
                                    snap->pools[i].util_pct, s->pool_pct));
            break;   /* 同一个指标只提示一次 */
        }
    }

    if (snap->cpu_temp_c > 0.0 && snap->cpu_temp_c >= s->temp_c) {
        add_hit(hits, &count, ALERT_TEMP,
                level_high(snap->cpu_temp_c, s->temp_c), "TEMP",
                snap->cpu_temp_c, s->temp_c,
                g_strdup_printf(_("CPU 温度 %.1f℃（阈值 %.0f℃）"),
                                snap->cpu_temp_c, s->temp_c));
    }

    if (snap->battery_present != 0 && snap->battery_pct >= 0.0 &&
        snap->battery_pct <= s->battery_pct &&
        (snap->battery_flags & FMON_BATT_DISCHARGING) != 0) {
        add_hit(hits, &count, ALERT_BATTERY,
                level_low(snap->battery_pct, s->battery_pct), "BATTERY",
                snap->battery_pct, s->battery_pct,
                g_strdup_printf(_("电池电量仅 %.0f%%（阈值 %.0f%%）"),
                                snap->battery_pct, s->battery_pct));
    }

    if (count == 0)
        return NULL;

    int64_t now = g_get_monotonic_time() / 1000;

    for (int i = 0; i < count; i++) {
        int idx = (int)hits[i].kind;

        /* 同一指标 10 分钟内只记录/通知一次，避免每秒刷一条 */
        if (now - g_last_fired[idx] < ALERT_COOLDOWN_MS)
            continue;

        g_last_fired[idx] = now;

        /* 静默只影响桌面通知，历史照常记录 */
        alert_log_append(&hits[i]);

        if (!alerts_is_silenced())
            send_notification((hits[i].level == ALERT_LEVEL_CRITICAL)
                                  ? "fmon 严重告警"
                                  : "fmon 告警",
                              hits[i].text);
    }

    /* 汇总成一行给界面提示用 */
    GString *summary = g_string_new(NULL);

    for (int i = 0; i < count; i++) {
        if (i > 0)
            g_string_append(summary, "；");
        if (hits[i].level == ALERT_LEVEL_CRITICAL)
            g_string_append(summary, "【严重】");
        g_string_append(summary, hits[i].text);
        g_free(hits[i].text);
    }

    return g_string_free(summary, FALSE);
}

/* ===========================================================================
 * 设置对话框
 * =========================================================================== */
typedef struct {
    AlertSettings *settings;
    GtkWidget     *win;
    GtkWidget     *chk_enabled;
    GtkWidget     *spin[6];
} AlertDialog;

static void on_alert_save(GtkButton *btn, gpointer data)
{
    (void)btn;

    AlertDialog   *dlg = data;
    AlertSettings *s   = dlg->settings;

    s->enabled = gtk_check_button_get_active(
        GTK_CHECK_BUTTON(dlg->chk_enabled));
    s->cpu_pct     = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dlg->spin[0]));
    s->mem_pct     = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dlg->spin[1]));
    s->swap_pct    = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dlg->spin[2]));
    s->pool_pct    = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dlg->spin[3]));
    s->temp_c      = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dlg->spin[4]));
    s->battery_pct = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dlg->spin[5]));

    alerts_save(s);

    gtk_window_destroy(GTK_WINDOW(dlg->win));
}

static void on_alert_cancel(GtkButton *btn, gpointer data)
{
    (void)btn;

    AlertDialog *dlg = data;

    gtk_window_destroy(GTK_WINDOW(dlg->win));
}

/* 窗口销毁时统一释放对话框上下文（保存/取消/点关闭都会走到这里） */
static gboolean on_alert_close(GtkWindow *win, gpointer data)
{
    (void)win;

    AlertDialog *dlg = data;

    if (dlg->win != NULL)
        g_object_set_data(G_OBJECT(dlg->win), "alert-dlg", NULL);

    g_free(dlg);
    return FALSE;
}

static void on_test_notify(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    alerts_test_notify();
}

static GtkWidget *make_spin(double value, double max)
{
    GtkAdjustment *adj = gtk_adjustment_new(value, 1.0, max, 1.0, 5.0, 0.0);
    GtkWidget     *sp  = gtk_spin_button_new(adj, 1.0, 0);

    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(sp), TRUE);
    gtk_widget_set_hexpand(sp, TRUE);
    return sp;
}

static void open_alert_dialog(GtkWindow *parent, AlertSettings *s)
{
    AlertDialog *dlg = g_new0(AlertDialog, 1);

    dlg->settings = s;
    dlg->win      = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg->win), _("告警设置"));
    gtk_window_set_default_size(GTK_WINDOW(dlg->win), 430, -1);
    gtk_window_set_transient_for(GTK_WINDOW(dlg->win), parent);
    gtk_window_set_modal(GTK_WINDOW(dlg->win), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vbox, 18);
    gtk_widget_set_margin_bottom(vbox, 18);
    gtk_widget_set_margin_start(vbox, 18);
    gtk_widget_set_margin_end(vbox, 18);

    dlg->chk_enabled = gtk_check_button_new_with_label(_("启用阈值告警（桌面通知）"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dlg->chk_enabled), s->enabled);
    gtk_box_append(GTK_BOX(vbox), dlg->chk_enabled);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    const char *labels[] = { "CPU 使用率超过 (%)",
                             "内存使用率（扣除可回收）超过 (%)",
                             "Swap 使用率超过 (%)", "ZFS 池使用率超过 (%)",
                             "CPU 温度超过 (℃)", "电池低于 (%)" };
    double values[] = { s->cpu_pct, s->mem_pct, s->swap_pct, s->pool_pct,
                        s->temp_c, s->battery_pct };
    double maxs[]   = { 100.0, 100.0, 100.0, 100.0, 120.0, 100.0 };

    for (int i = 0; i < 6; i++) {
        GtkWidget *lbl = gtk_label_new(labels[i]);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);

        dlg->spin[i] = make_spin(values[i], maxs[i]);

        gtk_grid_attach(GTK_GRID(grid), lbl, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), dlg->spin[i], 1, i, 1, 1);
    }

    gtk_box_append(GTK_BOX(vbox), grid);

    GtkWidget *hint = gtk_label_new(_("同一指标 10 分钟内只通知一次；超过阈值的项目会显示在窗口右上角。"));
    gtk_widget_add_css_class(hint, "small-text");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), hint);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);

    GtkWidget *btn_test   = gtk_button_new_with_label(_("测试通知"));
    GtkWidget *btn_cancel = gtk_button_new_with_label(_("取消"));
    GtkWidget *btn_save   = gtk_button_new_with_label(_("保存"));

    gtk_widget_add_css_class(btn_save, "suggested-action");

    g_signal_connect(btn_test, "clicked", G_CALLBACK(on_test_notify), NULL);
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_alert_cancel), dlg);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_alert_save), dlg);

    gtk_box_append(GTK_BOX(actions), btn_test);
    gtk_box_append(GTK_BOX(actions), btn_cancel);
    gtk_box_append(GTK_BOX(actions), btn_save);
    gtk_box_append(GTK_BOX(vbox), actions);

    gtk_window_set_child(GTK_WINDOW(dlg->win), vbox);
    g_signal_connect(dlg->win, "close-request", G_CALLBACK(on_alert_close), dlg);
    gtk_window_present(GTK_WINDOW(dlg->win));
}

static void on_alerts_button_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    GtkWindow *parent = GTK_WINDOW(data);
    if (parent == NULL)
        return;

    AlertSettings *s = g_object_get_data(G_OBJECT(parent), "alert-settings");
    if (s == NULL)
        return;

    open_alert_dialog(parent, s);
}

GtkWidget *alerts_create_button(GtkWindow *parent, AlertSettings *settings)
{
    /* 设置结构体挂在主窗口上，按钮回调通过它取用 */
    g_object_set_data(G_OBJECT(parent), "alert-settings", settings);

    GtkWidget *btn = gtk_button_new_with_label(_("告警设置"));
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_alerts_button_clicked),
                     parent);

    return btn;
}
