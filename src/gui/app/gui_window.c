#include "gui_window.h"
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
#include "rctl_view.h"
#include "combo_chart.h"
#include "font_setup.h"
#include "gui_chart.h"
#include "gui_state.h"
#include "theme.h"
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

static void gui_nav_switch_page(const char *page_id)
{
    if (g_stack != NULL && page_id != NULL) {
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
static gchar *g_theme_start = NULL;  /* --theme 指定的起始主题 */
static int    g_gpu_start   = 0;     /* --gpu 指定的启动显卡 */

/* 重建 GPU 下拉模型时会触发 notify::selected（选中项被重置为 0），
 * 必须用标志区分“用户切换”和“程序重建”，否则会覆盖用户/命令行选择。 */

/* ===========================================================================
 * 快照访问
 * ========================================================================= */




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

    (void)user_data;

    if (g_main_window != NULL) {
        gtk_window_present(g_main_window);
        return;
    }

    gui_css_load();

    g_main_window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_icon_name(g_main_window, "org.freebsd.fmon");

    /* 指标页曲线绑定到快照中的历史数组 */
    gui_metric_curves_bind(&g_snap);

    /* 告警阈值（~/.config/fmon/alerts.ini） */
    alerts_load(&g_alerts);
    gtk_window_set_title(g_main_window, _("FreeBSD 监控中心"));
    /* 不记忆窗口状态：每次使用固定的大尺寸，打开就是首页 */
    gtk_window_set_default_size(g_main_window, 1500, 920);
    gtk_widget_set_size_request(GTK_WIDGET(g_main_window), 1200, 760);
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
    gtk_window_set_child(g_main_window, main_hbox);

    /* ---------- 侧边栏 ---------- */
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 216, -1);

    GtkWidget *sidebar_list = gui_nav_sidebar_new();
    GtkWidget *nav_scroll = gtk_scrolled_window_new();

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(nav_scroll), FALSE);
    gtk_widget_set_vexpand(nav_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(nav_scroll), sidebar_list);
    gtk_box_append(GTK_BOX(sidebar), nav_scroll);
    gtk_box_append(GTK_BOX(main_hbox), sidebar);

    /* 二级列表：当前分组的子项（首次点击分组时滑出） */
    GtkWidget *sub_revealer = gui_nav_revealer_new();

    gtk_box_append(GTK_BOX(main_hbox), sub_revealer);

    /* ---------- 内容区 ---------- */
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(content, "content-area");
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);
    gtk_box_append(GTK_BOX(main_hbox), content);

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title_box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(title_box, TRUE);

    /* 页面名已经在侧边栏高亮显示，这里不再重复一个大标题，
     * 只保留系统信息那一行。 */
    /* 先用空格占位：数据到达前标签也有行高，避免整页内容突然下移 */
    g_header_sub = gtk_label_new(" ");
    gtk_widget_add_css_class(g_header_sub, "subheader");
    gtk_widget_set_size_request(g_header_sub, -1, 18);
    gtk_widget_set_halign(g_header_sub, GTK_ALIGN_START);

    gtk_widget_set_valign(g_header_sub, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(title_box), g_header_sub);
    gtk_box_append(GTK_BOX(header_box), title_box);

    g_alert_label = gtk_label_new(_("⚠ 告警"));
    gtk_widget_add_css_class(g_alert_label, "danger");
    gtk_widget_set_valign(g_alert_label, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(g_alert_label, FALSE);
    gtk_box_append(GTK_BOX(header_box), g_alert_label);

    /* 主题：两段式开关（浅色 / 深色） */
    gtk_box_append(GTK_BOX(header_box), gui_theme_ui_switch());

    /* 导出：主按钮（蓝色） */
    GtkWidget *btn_export = gtk_button_new_with_label(_("导出数据"));
    gtk_widget_set_valign(btn_export, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(btn_export, "suggested-action");
    g_signal_connect(btn_export, "clicked", G_CALLBACK(gui_export_clicked),
                     NULL);
    gtk_box_append(GTK_BOX(header_box), btn_export);

    /* 更多：关于 / 告警设置收进 “...” 菜单 */
    GtkWidget *more = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "fmon-menu-symbolic");
    gtk_widget_set_tooltip_text(more, _("更多"));
    gtk_widget_set_valign(more, GTK_ALIGN_CENTER);

    GtkWidget *pop = gtk_popover_new();
    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_widget_set_margin_top(pbox, 8);
    gtk_widget_set_margin_bottom(pbox, 8);
    gtk_widget_set_margin_start(pbox, 8);
    gtk_widget_set_margin_end(pbox, 8);

    GtkWidget *mi_about = gtk_button_new_with_label(_("关于"));

    gtk_widget_add_css_class(mi_about, "flat");
    g_signal_connect_swapped(mi_about, "clicked",
                             G_CALLBACK(fmon_about_show), g_main_window);
    gtk_box_append(GTK_BOX(pbox), mi_about);

    GtkWidget *mi_alerts = alerts_create_button(g_main_window, &g_alerts);

    gtk_widget_add_css_class(mi_alerts, "flat");
    gtk_box_append(GTK_BOX(pbox), mi_alerts);

    GtkWidget *mi_auto = gtk_check_button_new_with_label(
        _("百分比图表自动量程"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(mi_auto),
                                gui_chart_auto_range());
    gtk_widget_add_css_class(mi_auto, "flat");
    gtk_widget_set_tooltip_text(
        mi_auto, _("默认 0–100%；打开后按当前窗口数据缩放，轴标签仍显示真实范围"));
    g_signal_connect(mi_auto, "toggled", G_CALLBACK(on_auto_range_toggled),
                     NULL);
    gtk_box_append(GTK_BOX(pbox), mi_auto);

    gtk_popover_set_child(GTK_POPOVER(pop), pbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(more), pop);
    gtk_box_append(GTK_BOX(header_box), more);
    gtk_box_append(GTK_BOX(content), header_box);

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










