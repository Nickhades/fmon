#include "gui_window.h"
#include "gui_language_switch.h"
#include "gui_window_state.h"
#include "gui_export.h"
#include "gui_runtime.h"
#include "gui_theme_ui.h"
#include "gui_window_events.h"
#include "gui_pages_layout.h"
#include "gui_page_state.h"
#include "gui_page_build.h"
#include "gui_refresh.h"
#include "collector_thread.h"

/*
 * main.c - fmon 图形界面主窗口
 *
 * 与旧版本相比的结构性改动：
 *   1. 不再直接把共享内存映射指针交给各个控件。每次刷新先
 *      fmon_shm_snapshot() 拷贝一份一致快照（seqlock），所有页面只读快照，
 *      彻底消除“读到一半旧一半新”的数据撕裂。
 *   2. 打开共享内存前先校验大小 / magic / ABI，旧版本结构体不一致时会
 *      直接读越界触发 SIGBUS（目录里的 fmon-gui.core 很可能就是这个原因）。
 *   3. 增加守护进程离线 / 数据过期横幅，用户能分清“系统空闲”和“采集停了”。
 *   4. 所有定时器都在窗口关闭时移除，并置空全局窗口指针，
 *      避免旧版本“关窗后定时器仍在跑、访问已销毁控件”的 UAF。
 *   5. 新增：每核 CPU 柱状图、Top 进程、存储空间页、CPU/温度等
 *      全部使用真实采集数据。
 */

#include <glib/gi18n.h>

#include <errno.h>
#include <glib-unix.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/sysctl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <gtk/gtk.h>

#include "fmon_shm.h"
#include "about.h"
#include "alerts.h"
#include "alert_view.h"
#include "history_view.h"
#include "history/long_history.h"
#include "rctl_view.h"
#include "combo_chart.h"
#include "font_setup.h"
#include "gui_chart.h"
#include "gui_state.h"
#include "theme.h"
#include "language.h"
#include "gui_css.h"
#include "gui_coreview.h"
#include "gui_pages.h"
#include "gui_page_gpu.h"
#include "gui_nav.h"
#include "gui_page_net.h"
#include "gui_page_disk.h"
#include "gui_page_mem.h"
#include "gui_widgets.h"
#include "gui_util.h"
#include "log_view.h"
#include "network_view.h"
#include "process_view.h"
#include "service_view.h"
#include "storage_view.h"

/* gui_util.c 在需要父窗口时使用 */
GtkWindow *g_main_window = NULL;

static GtkStack *g_stack;
static gboolean g_presented;

static gboolean tick_once_cb(gpointer data);
static void     present_once(void);

static void on_auto_range_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;

    gui_chart_set_auto_range(gtk_toggle_button_get_active(btn));
    gui_pages_redraw_all();
}

static gboolean scroll_top_cb(gpointer data)
{
    (void)data;

    if (g_stack == NULL)
        return G_SOURCE_REMOVE;

    GtkWidget *w = gtk_stack_get_visible_child(g_stack);

    if (w != NULL && GTK_IS_SCROLLED_WINDOW(w)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(w));

        if (adj != NULL)
            gtk_adjustment_set_value(adj, 0.0);
    }

    return G_SOURCE_REMOVE;
}

static void gui_nav_switch_page(const char *page_id);

void fmon_gui_switch_page(const char *page_id)
{
    gui_nav_switch_page(page_id);
}

const char *gui_window_current_page(void)
{
    return (g_stack != NULL) ? gtk_stack_get_visible_child_name(g_stack)
                             : NULL;
}

static void gui_nav_switch_page(const char *page_id)
{
    if (g_stack != NULL && page_id != NULL) {
        gui_nav_select_page(page_id);
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), page_id);

        /* 新页面从顶部开始，再刷新一次被隐藏跳过的详情 */
        g_idle_add(scroll_top_cb, NULL);
        g_timeout_add(120, scroll_top_cb, NULL);
        g_timeout_add(400, scroll_top_cb, NULL);
        g_timeout_add(20, tick_once_cb, NULL);
    }
}

























/* CPU 页：每个逻辑核一个小方块，按使用率着色 */





static AlertSettings g_alerts;

static gchar *g_start_page = NULL;   /* --page 指定的启动页面 */
static gint64 g_long_next_refresh;
static GtkWidget *g_long_note;
static gchar *g_theme_start = NULL;  /* --theme 指定的起始主题 */
static int    g_gpu_start   = 0;     /* --gpu 指定的启动显卡 */

/* 重建 GPU 下拉模型时会触发 notify::selected（选中项被重置为 0），
 * 必须用标志区分“用户切换”和“程序重建”，否则会覆盖用户/命令行选择。 */

/* ===========================================================================
 * 快照访问
 * ========================================================================= */




static GtkDropDown *g_window_dd;
static gboolean     g_window_sync;

static void update_long_note(void)
{
    if (g_long_note == NULL)
        return;

    gtk_widget_set_visible(g_long_note, gui_long_history_active());
}

static void chart_window_notify(uint64_t us)
{
    if (g_window_dd == NULL)
        return;

    guint idx = 8;   /* 自定义 */
    uint64_t secs = us / 1000000ULL;

    if (gui_long_history_active()) {
        if (secs == 3600)
            idx = 4;
        else if (secs == 86400)
            idx = 5;
        else if (secs == 7 * 86400ULL)
            idx = 6;
        else if (secs == 30 * 86400ULL)
            idx = 7;
        else
            idx = 8;
    } else if (secs == 60) {
        idx = 0;
    } else if (secs == 300) {
        idx = 1;
    } else if (secs == 900) {
        idx = 2;
    } else if (secs == 1200) {
        idx = 3;
    }

    g_window_sync = TRUE;
    gtk_drop_down_set_selected(g_window_dd, idx);
    g_window_sync = FALSE;
}

static void on_chart_window_changed(GtkDropDown *dd, GParamSpec *pspec,
                                    gpointer data)
{
    (void)pspec;
    (void)data;

    if (g_window_sync)
        return;

    guint sel = gtk_drop_down_get_selected(dd);

    if (sel == 8)
        return;   /* 自定义：由框选/滚轮设置，不覆盖 */

    if (sel >= 4 && sel <= 7) {
        gint64 range = (sel == 4) ? 3600
                       : (sel == 5) ? 86400
                       : (sel == 6) ? 7 * 86400
                                    : 30 * 86400;

        if (!gui_long_history_enter(range)) {
            fmon_gui_show_error(g_main_window, _("长历史不可用"),
                                _("没有可用的历史金字塔文件。"));
            g_window_sync = TRUE;
            gtk_drop_down_set_selected(dd,
                                       gui_long_history_active() ? 4 : 3);
            g_window_sync = FALSE;
            return;
        }

        gui_chart_set_view_end_us(0);
        gui_chart_set_window_us((uint64_t)range * 1000000ULL);
        g_long_next_refresh = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;
        update_long_note();
        gui_pages_refresh(&g_snap);
        return;
    }

    if (gui_long_history_active()) {
        gui_long_history_leave();
        update_long_note();
    }

    uint64_t us;

    switch (sel) {
    case 0:
        us = 60ULL * 1000000ULL;
        break;
    case 1:
        us = 5ULL * 60ULL * 1000000ULL;
        break;
    case 2:
        us = 15ULL * 60ULL * 1000000ULL;
        break;
    default:
        us = 20ULL * 60ULL * 1000000ULL;
        break;
    }

    gui_chart_set_view_end_us(0);
    gui_chart_set_window_us(us);
    gui_pages_refresh(&g_snap);
}

static gboolean on_ui_tick(gpointer data)
{
    (void)data;

    if (g_main_window == NULL)
        return G_SOURCE_REMOVE;

    gui_runtime_snapshot();
    gui_runtime_banner();
    gui_runtime_header(NULL, NULL);   /* 顶部系统信息随 uptime 刷新 */
    gui_metric_curves_sync(&g_snap);
    gui_runtime_alert_indicator();

    if (gui_long_history_active() &&
        g_get_monotonic_time() >= g_long_next_refresh) {
        gui_long_history_refresh();
        g_long_next_refresh = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

        if (!gui_long_history_active()) {
            g_window_sync = TRUE;
            gtk_drop_down_set_selected(g_window_dd, 3);
            g_window_sync = FALSE;
            gui_chart_set_window_us(20ULL * 60ULL * 1000000ULL);
            update_long_note();
        }
    }

    gui_pages_refresh(&g_snap);

    return G_SOURCE_CONTINUE;
}

static gboolean tick_once_cb(gpointer data)
{
    on_ui_tick(data);
    return G_SOURCE_REMOVE;
}

static void present_once(void)
{
    if (g_presented || g_main_window == NULL)
        return;

    g_presented = TRUE;
    gtk_window_present(g_main_window);

    gui_window_state_center_after_present();

    /* 首次映射后把当前页滚到顶部，避免数据填入时停在半截 */
    g_timeout_add(120, scroll_top_cb, NULL);
    g_timeout_add(400, scroll_top_cb, NULL);
}

/* 第一帧数据就绪：先显示窗口，再立刻刷新一次（此时控件已映射） */
static void first_frame_ready(void)
{
    present_once();
    g_timeout_add(20, tick_once_cb, NULL);
}

static gboolean present_fallback(gpointer data)
{
    (void)data;
    present_once();
    return G_SOURCE_REMOVE;
}

/* ===========================================================================
 * CSV 导出
 * ========================================================================= */


/* 支持 `fmon-gui --page zfs` 直接打开指定页面 */
gint gui_window_handle_options(GApplication *app, GVariantDict *options,
                              gpointer user_data)
{
    (void)app;

    const gchar *page = NULL;

    if (g_variant_dict_lookup(options, "page", "&s", &page) && page != NULL) {
        g_free(g_start_page);
        g_start_page = g_strdup(page);
    }

    gint gpu = 0;
    if (g_variant_dict_lookup(options, "gpu", "i", &gpu) &&
        gpu >= 0 && gpu < FMON_MAX_GPUS)
        g_gpu_start = gpu;

    const gchar *theme = NULL;

    if (g_variant_dict_lookup(options, "theme", "&s", &theme) && theme != NULL) {
        g_free(g_theme_start);
        g_theme_start = g_strdup(theme);
    }

    gboolean show_version = FALSE;
    if (g_variant_dict_lookup(options, "version", "b", &show_version) &&
        show_version) {
        g_print("fmon-gui %s (共享内存 ABI %u)\n", FMON_VERSION_STRING,
                FMON_ABI_VERSION);
        return 0;   /* 已处理，不启动界面 */
    }

    return -1;   /* 继续默认处理流程 */
}


void gui_window_activate(GtkApplication *app, gpointer user_data)
{
    /* 自带图标：不依赖桌面图标主题。KDE/Breeze 等主题缺 `*-symbolic`
     * 名称时，导航栏会显示“缺图”占位符。 */
    GdkDisplay *display = gdk_display_get_default();

    if (display != NULL)
        gtk_icon_theme_add_resource_path(
            gtk_icon_theme_get_for_display(display), "/org/fmon/icons");

    /* 自绘应用图标：先走资源，安装后同名的 hicolor SVG 也可被任务栏/
     * 桌面环境读取。不依赖任何系统图标主题。 */
    gtk_window_set_default_icon_name("org.freebsd.fmon");

    gui_theme_ui_init(g_theme_start);
    gui_chart_settings_init();
    gui_chart_set_window_us(20ULL * 60ULL * 1000000ULL);

    (void)user_data;

    if (g_main_window != NULL) {
        gtk_window_present(g_main_window);
        return;
    }

    gui_css_load();
    gui_window_state_init();

    g_main_window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_icon_name(g_main_window, "org.freebsd.fmon");

    /* 告警阈值必须先加载，图表绑定曲线时才能拿到阈值指针。 */
    alerts_load(&g_alerts);

    /* 指标页曲线绑定到快照中的历史数组 */
    gui_metric_curves_bind(&g_snap);
    gtk_window_set_title(g_main_window, _("FreeBSD 监控中心"));

    gui_window_state_attach(g_main_window);
    g_signal_connect(g_main_window, "close-request",
                     G_CALLBACK(gui_window_close_request), NULL);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(gui_window_key_pressed), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_main_window), keys);

    /* 先把数据准备好，避免第一帧全是 0 */
    gui_runtime_snapshot();

    /* --gpu 指定默认显示的显卡（多卡机器上很有用） */
    gui_state_set_gpu_index(g_gpu_start);

    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(main_hbox), FALSE);
    gtk_window_set_child(g_main_window, main_hbox);

    if (fmon_theme_is_dark())
        gtk_widget_add_css_class(GTK_WIDGET(g_main_window), "fmon-dark");

    /* ---------- 侧边栏 ---------- */
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 216, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);

    GtkWidget *sidebar_list = gui_nav_sidebar_new();
    GtkWidget *nav_scroll = gtk_scrolled_window_new();

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(nav_scroll), FALSE);
    gtk_widget_set_vexpand(nav_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(nav_scroll), sidebar_list);
    gtk_box_append(GTK_BOX(sidebar), nav_scroll);

    /* 语言切换固定在侧边栏底部；切换后 exec 自身，整窗重建。 */
    GtkWidget *lang_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *lang_zh = gtk_toggle_button_new_with_label(
        fmon_language_get() == FMON_LANGUAGE_EN ? "Chinese" : "中文");
    GtkWidget *lang_en = gtk_toggle_button_new_with_label("EN");

    gtk_widget_add_css_class(lang_box, "lang-switch");
    gtk_widget_add_css_class(lang_zh, "lang-button");
    gtk_widget_add_css_class(lang_en, "lang-button");
    gtk_widget_set_hexpand(lang_zh, FALSE);
    gtk_widget_set_hexpand(lang_en, FALSE);
    gtk_widget_set_size_request(lang_zh, 96, -1);
    gtk_widget_set_size_request(lang_en, 96, -1);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(lang_zh),
                                GTK_TOGGLE_BUTTON(lang_en));
    gtk_widget_set_tooltip_text(lang_box,
                                _("界面语言（切换后自动重启）"));

    g_signal_connect(lang_zh, "toggled", G_CALLBACK(gui_language_toggled),
                     GINT_TO_POINTER(FMON_LANGUAGE_ZH));
    g_signal_connect(lang_en, "toggled", G_CALLBACK(gui_language_toggled),
                     GINT_TO_POINTER(FMON_LANGUAGE_EN));

    gui_language_set_syncing(TRUE);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(fmon_language_get() == FMON_LANGUAGE_EN ? lang_en
                                                                  : lang_zh),
        TRUE);
    gui_language_set_syncing(FALSE);

    gtk_box_append(GTK_BOX(lang_box), lang_zh);
    gtk_box_append(GTK_BOX(lang_box), lang_en);
    gtk_box_append(GTK_BOX(sidebar), lang_box);

    gtk_box_append(GTK_BOX(main_hbox), sidebar);

    /* 子分组改成页面顶部标签条，不再占用第二列侧边栏。
     * 横向多出 176px，全部留给数据卡片和图表。 */

    /* ---------- 内容区 ---------- */
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(content, "content-area");
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);
    gtk_box_append(GTK_BOX(main_hbox), content);

    /* 页面顶部子分组标签：CPU / 内存 / GPU / 历史记录 ... */
    GtkWidget *pagebar = gui_nav_pagebar_new();
    gtk_box_append(GTK_BOX(content), pagebar);

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title_box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(title_box, TRUE);

    /* 页面名已经在顶部标签高亮显示，这里不再重复一个大标题，
     * 只保留系统信息那一行。 */
    /* 先用空格占位：数据到达前标签也有行高，避免整页内容突然下移 */
    g_header_sub = gtk_label_new(" ");
    gtk_widget_add_css_class(g_header_sub, "subheader");
    gtk_widget_set_size_request(g_header_sub, -1, 18);
    gtk_widget_set_halign(g_header_sub, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(g_header_sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_header_sub), 64);

    gtk_widget_set_valign(g_header_sub, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(title_box), g_header_sub);
    gtk_box_append(GTK_BOX(header_box), title_box);

    g_alert_label = gtk_label_new(_("⚠ 告警"));
    gtk_widget_add_css_class(g_alert_label, "danger");
    gtk_widget_set_valign(g_alert_label, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(g_alert_label, FALSE);
    gtk_box_append(GTK_BOX(header_box), g_alert_label);

    /* 实时图时间窗口：1 / 5 / 15 / 20 分钟（SHM 环形缓冲上限 20 分钟）。
     * 1h/24h 请使用“历史”页，避免把 86400 点塞进共享内存。 */
    const char *const windows[] = {
        _("1 分钟"), _("5 分钟"), _("15 分钟"), _("20 分钟"),
        _("1 小时"), _("24 小时"), _("7 天"), _("30 天"), _("自定义"), NULL
    };
    GtkWidget *window_dd = gtk_drop_down_new_from_strings(windows);

    g_window_dd = GTK_DROP_DOWN(window_dd);
    gui_chart_set_window_notify(chart_window_notify);
    gtk_widget_set_valign(window_dd, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(window_dd, 108, -1);
    gtk_widget_set_tooltip_text(
        window_dd, _("实时图 20 分钟内；1 小时/24 小时使用 1 分钟金字塔"));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(window_dd), 3);
    g_signal_connect(window_dd, "notify::selected",
                     G_CALLBACK(on_chart_window_changed), NULL);
    gtk_box_append(GTK_BOX(header_box), window_dd);

    /* 主题：两段式开关（浅色 / 深色） */
    gtk_box_append(GTK_BOX(header_box), gui_theme_ui_switch());

    /* 导出：主按钮（蓝色） */
    GtkWidget *btn_export = gtk_button_new_with_label(_("导出数据"));
    gtk_widget_set_valign(btn_export, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(btn_export, "suggested-action");
    g_signal_connect(btn_export, "clicked", G_CALLBACK(gui_export_clicked),
                     NULL);
    gtk_box_append(GTK_BOX(header_box), btn_export);

    /* 关于 / 告警 / 自动量程直接放在工具栏，不再收进 “...” 下拉。 */
    GtkWidget *btn_about = gtk_button_new_with_label(_("关于"));

    gtk_widget_add_css_class(btn_about, "flat");
    gtk_widget_add_css_class(btn_about, "header-action");
    gtk_widget_set_tooltip_text(btn_about, _("关于 fmon"));
    g_signal_connect_swapped(btn_about, "clicked",
                             G_CALLBACK(fmon_about_show), g_main_window);

    GtkWidget *btn_alerts = alerts_create_button(g_main_window, &g_alerts);

    gtk_widget_add_css_class(btn_alerts, "flat");
    gtk_widget_add_css_class(btn_alerts, "header-action");
    gtk_button_set_label(GTK_BUTTON(btn_alerts), _("告警"));
    gtk_widget_set_tooltip_text(btn_alerts, _("告警设置"));

    GtkWidget *auto_range = gtk_check_button_new_with_label(_("自动量程"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(auto_range),
                                gui_chart_auto_range());
    gtk_widget_add_css_class(auto_range, "flat");
    gtk_widget_add_css_class(auto_range, "header-action");
    gtk_widget_set_tooltip_text(
        auto_range, _("百分比图表默认 0–100%；打开后按当前窗口数据缩放，"
                      "轴标签仍显示真实范围"));
    g_signal_connect(auto_range, "toggled", G_CALLBACK(on_auto_range_toggled),
                     NULL);

    gtk_box_append(GTK_BOX(content), header_box);

    GtkWidget *quick_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(quick_row, "quick-actions");
    gtk_widget_set_halign(quick_row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(quick_row), btn_about);
    gtk_box_append(GTK_BOX(quick_row), btn_alerts);
    gtk_box_append(GTK_BOX(quick_row), auto_range);
    gtk_box_append(GTK_BOX(content), quick_row);

    /* 离线 / 过期横幅 */
    g_banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(g_banner, "banner-warning");

    GtkWidget *warn_icon = gtk_image_new_from_icon_name("fmon-alerts-symbolic");
    gtk_box_append(GTK_BOX(g_banner), warn_icon);

    g_banner_label = gtk_label_new("");

    gui_runtime_bind(g_header_sub, g_banner, g_banner_label, g_alert_label,
                     &g_alerts);
    gtk_label_set_wrap(GTK_LABEL(g_banner_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_banner_label), 0.0f);
    gtk_widget_set_hexpand(g_banner_label, TRUE);
    gtk_box_append(GTK_BOX(g_banner), g_banner_label);

    gtk_widget_set_visible(g_banner, FALSE);
    gtk_box_append(GTK_BOX(content), g_banner);

    g_long_note = gtk_label_new(
        _("长历史模式（ZFS/GPU 部分聚合）：CPU / 内存 / 网络 / "
          "磁盘读写/温度、ZFS 池使用率/ARC 命中率/L2ARC/ZFS IO、"
          "GPU 利用率/温度/显存使用；ZFS 容量构成等仍为实时。"));
    gtk_widget_add_css_class(g_long_note, "small-text");
    gtk_label_set_wrap(GTK_LABEL(g_long_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_long_note), 0.0f);
    gtk_widget_set_visible(g_long_note, FALSE);
    gtk_box_append(GTK_BOX(content), g_long_note);

    g_stack = gui_pages_layout_build(content);

    /* 左侧只放分组 */
    gui_nav_build_groups();

    gui_nav_set_switch(gui_nav_switch_page);

    if (g_start_page == NULL && g_gpu_start > 0) {
        gchar *gpupage = g_strdup_printf("gpu%d", g_gpu_start);

        gui_nav_start(gpupage);
        g_free(gpupage);
    } else {
        gui_nav_start(g_start_page);
    }

    gui_runtime_banner();
    
    /* 统一 1 秒刷新：所有曲线每秒一个点、每秒绘制一次，详情同步更新。 */
    gui_runtime_start_timer(on_ui_tick);

    /* 常驻采集线程（sysctl 类）；FMON_LEGACY_COLLECT=1 时不起 */
    gui_collector_set_ready_cb(first_frame_ready);
    gui_collector_start();

    g_unix_signal_add(SIGTERM, gui_window_quit_signal, NULL);
    g_unix_signal_add(SIGINT, gui_window_quit_signal, NULL);

    /* 等第一帧数据再显示，避免先出现空白页；最多等 700ms 兜底 */
    if (gui_collector_active())
        g_timeout_add(700, present_fallback, NULL);
    else
        present_once();
}
