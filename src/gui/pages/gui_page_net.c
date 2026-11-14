/*
 * gui_page_net.c - 网络接口详情卡片与刷新
 *
 * 由 main.c 拆出：数据来源于 gui_state_snapshot() 的一致快照。
 */

#include "gui_page_net.h"

#include "gui_state.h"
#include "gui_util.h"
#include "gui_widgets.h"
#include "net_stats.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

static GtkWidget *s_grid = NULL;

static const char *iface_type_text(uint32_t t)
{
    switch (t) {
    case FMON_LINK_ETHER:   return "以太网";
    case FMON_LINK_WIFI:    return "Wi-Fi";
    case FMON_LINK_LOOP:    return "回环";
    case FMON_LINK_VIRTUAL: return "虚拟";
    default:                return "未知";
    }
}


static GtkWidget *s_if_grid   = NULL;
static GtkWidget *s_tcp_line  = NULL;
static gchar     *s_if_sig    = NULL;
static GtkWidget *s_if_name[NET_STATS_MAX_IFS];
static GtkWidget *s_if_type[NET_STATS_MAX_IFS];
static GtkWidget *s_if_addr[NET_STATS_MAX_IFS];
static GtkWidget *s_wifi_line = NULL;
static GtkWidget *s_if_mtu[NET_STATS_MAX_IFS];
static GtkWidget *s_if_state[NET_STATS_MAX_IFS];
static GtkWidget *s_if_err[NET_STATS_MAX_IFS];
static GtkWidget *s_if_drop[NET_STATS_MAX_IFS];
static int        s_if_count  = 0;

/* 按接口名归类：无线 / USB 共享 / 虚拟 / 回环 / 以太网 */
static const char *iface_kind(const char *name)
{
    if (g_str_has_prefix(name, "wlan"))
        return "无线";
    if (g_str_has_prefix(name, "ue") || g_str_has_prefix(name, "urndis") ||
        g_str_has_prefix(name, "cdce") || g_str_has_prefix(name, "ipheth") ||
        g_str_has_prefix(name, "u3g"))
        return "USB 共享";
    if (g_str_has_prefix(name, "tap") || g_str_has_prefix(name, "bridge") ||
        g_str_has_prefix(name, "epair") || g_str_has_prefix(name, "vmnet"))
        return "虚拟";
    if (g_str_has_prefix(name, "lo"))
        return "回环";
    return "以太网";
}

static GtkWidget *cell(const char *text, const char *css, int width)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_halign(l, GTK_ALIGN_START);

    if (width > 0)
        gtk_widget_set_size_request(l, width, -1);
    return l;
}

static GtkWidget *s_nic_box = NULL;
static gchar     *s_nic_sig = NULL;

GtkWidget *gui_net_nic_header(void)
{
    s_nic_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(s_nic_box, GTK_ALIGN_END);
    gtk_widget_set_valign(s_nic_box, GTK_ALIGN_START);
    return s_nic_box;
}

void gui_net_nic_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (s_nic_box == NULL)
        return;

    GString *sig = g_string_new(NULL);

    for (uint32_t i = 0; i < snap->nic_count && i < 4; i++)
        g_string_append_printf(sig, "%s;", snap->nic_models[i]);

    if (g_strcmp0(sig->str, s_nic_sig) == 0) {
        g_string_free(sig, TRUE);
        return;
    }

    g_free(s_nic_sig);
    s_nic_sig = g_string_free(sig, FALSE);

    GtkWidget *child = NULL;

    while ((child = gtk_widget_get_first_child(s_nic_box)) != NULL)
        gtk_box_remove(GTK_BOX(s_nic_box), child);

    if (snap->nic_count == 0) {
        GtkWidget *l = cell(_("未检测到网卡硬件"), "hw-model", 0);

        gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(s_nic_box), l);
        return;
    }

    for (uint32_t i = 0; i < snap->nic_count && i < 4; i++) {
        GtkWidget *l = cell(snap->nic_models[i], "hw-model", 0);

        gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        /* 小字号 + 固定宽度换行：型号再长也能完整显示，不挤占标题 */
        gtk_widget_set_size_request(l, 640, -1);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_RIGHT);
        gtk_box_append(GTK_BOX(s_nic_box), l);
    }
}

GtkWidget *gui_net_health_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    gtk_widget_set_margin_top(box, 10);

    GtkWidget *t = cell(_("链路与错误"), "detail-title", 0);

    gtk_box_append(GTK_BOX(box), t);

    s_if_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(s_if_grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(s_if_grid), 14);
    gtk_widget_set_margin_top(s_if_grid, 2);
    gtk_box_append(GTK_BOX(box), s_if_grid);

    s_wifi_line = cell("", "group-desc", 0);
    gtk_widget_set_margin_top(s_wifi_line, 2);
    gtk_widget_set_visible(s_wifi_line, FALSE);
    gtk_box_append(GTK_BOX(box), s_wifi_line);

    GtkWidget *tcp_title = cell(_("TCP 健康度"), "detail-title", 0);

    gtk_widget_set_margin_top(tcp_title, 10);
    gtk_box_append(GTK_BOX(box), tcp_title);

    s_tcp_line = cell("—", "group-desc", 0);
    gtk_box_append(GTK_BOX(box), s_tcp_line);

    return box;
}

static void rebuild_if_grid(const NetStats *ns)
{
    GtkWidget *child = NULL;

    while ((child = gtk_widget_get_first_child(s_if_grid)) != NULL)
        gtk_grid_remove(GTK_GRID(s_if_grid), child);

    static const char *const head[] = {
        "接口", "类型", "地址", "状态", "MTU", "错误 收/发", "丢包",
    };
    static const int head_w[] = { 88, 76, 136, 66, 56, 120, 60 };

    for (int c = 0; c < 7; c++) {
        GtkWidget *h = cell(head[c], "detail-key", head_w[c]);

        gtk_grid_attach(GTK_GRID(s_if_grid), h, c, 0, 1, 1);
    }

    s_if_count = ns->nifs;

    for (int i = 0; i < ns->nifs; i++) {
        int r = i + 1;

        s_if_name[i]  = cell(ns->ifs[i].name, "detail-value", 88);
        s_if_type[i]  = cell(iface_kind(ns->ifs[i].name), "group-desc", 76);
        s_if_addr[i]  = cell("", "group-desc", 136);
        s_if_state[i] = cell("", NULL, 66);
        s_if_mtu[i]   = cell("", "group-desc", 56);
        s_if_err[i]   = cell("", "group-desc", 120);
        s_if_drop[i]  = cell("", "group-desc", 60);

        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_name[i], 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_type[i], 1, r, 1, 1);
        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_addr[i], 2, r, 1, 1);
        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_state[i], 3, r, 1, 1);
        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_mtu[i], 4, r, 1, 1);
        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_err[i], 5, r, 1, 1);
        gtk_grid_attach(GTK_GRID(s_if_grid), s_if_drop[i], 6, r, 1, 1);
    }
}

void gui_net_health_update(void)
{
    const NetStats *ns = net_stats_get();

    if (ns == NULL || s_if_grid == NULL)
        return;

    /* 接口集合或上下线状态变化才重建行，数值每秒就地刷新 */
    GString *sig = g_string_new(NULL);

    for (int i = 0; i < ns->nifs; i++)
        g_string_append_printf(sig, "%s|%d;", ns->ifs[i].name,
                               ns->ifs[i].up ? 1 : 0);

    if (g_strcmp0(sig->str, s_if_sig) != 0) {
        g_free(s_if_sig);
        s_if_sig = g_string_free(sig, FALSE);
        rebuild_if_grid(ns);
    } else {
        g_string_free(sig, TRUE);
    }

    for (int i = 0; i < ns->nifs; i++) {
        const NetIfaceStat *f = &ns->ifs[i];

        gtk_label_set_text(GTK_LABEL(s_if_addr[i]),
                           (f->addr[0] != '\0') ? f->addr : "—");

        gchar *mtu = g_strdup_printf("%u", f->mtu);

        gtk_label_set_text(GTK_LABEL(s_if_mtu[i]), mtu);
        g_free(mtu);

        if (f->up)
            gtk_label_set_markup(GTK_LABEL(s_if_state[i]),
                                 "<span foreground=\"#2e7d32\">已连接</span>");
        else
            gtk_label_set_markup(GTK_LABEL(s_if_state[i]),
                                 "<span foreground=\"#c62828\">未连接</span>");

        gchar *err = g_strdup_printf("%llu / %llu",
                                     (unsigned long long)f->ierrors,
                                     (unsigned long long)f->oerrors);

        gtk_label_set_text(GTK_LABEL(s_if_err[i]), err);
        g_free(err);

        gchar *drop = g_strdup_printf("%llu",
                                      (unsigned long long)f->iqdrops);

        gtk_label_set_text(GTK_LABEL(s_if_drop[i]), drop);
        g_free(drop);
    }

    /* 无线接口：SSID / 信道 / 协议（有 wlan 接口时才显示） */
    GString *wifi = g_string_new(NULL);

    for (int i = 0; i < ns->nifs; i++) {
        const NetIfaceStat *f = &ns->ifs[i];

        if (!f->wireless)
            continue;

        if (wifi->len > 0)
            g_string_append(wifi, " · ");
        g_string_append_printf(wifi, "%s：SSID %s · 信道 %d · %s", f->name,
                               (f->ssid[0] != '\0') ? f->ssid : "—",
                               f->channel,
                               (f->media[0] != '\0') ? f->media : "—");
    }

    gtk_label_set_text(GTK_LABEL(s_wifi_line), wifi->str);
    gtk_widget_set_visible(s_wifi_line, wifi->len > 0);
    g_string_free(wifi, TRUE);

    gchar *tcp = g_strdup_printf(
        _("重传率 %.2f%%（重传 %llu / 发送 %llu） · 监听队列溢出 %llu · "
          "连接尝试 %llu / 关闭 %llu"),
        ns->retrans_pct, (unsigned long long)ns->tcp_rexmit,
        (unsigned long long)ns->tcp_sndtotal,
        (unsigned long long)ns->tcp_listendrop,
        (unsigned long long)ns->tcp_connattempt,
        (unsigned long long)ns->tcp_closed);

    gtk_label_set_text(GTK_LABEL(s_tcp_line), tcp);
    g_free(tcp);
}

GtkWidget *gui_net_details_card(void)
{
    return gui_info_card(_("网络接口详情"), &s_grid);
}

void gui_net_details_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (s_grid == NULL)
        return;

    GString *sig = g_string_new(NULL);

    for (uint32_t i = 0; i < snap->iface_count && i < FMON_MAX_IFACES; i++) {
        const FmonIfaceInfo *f = &snap->ifaces[i];

        g_string_append_printf(sig, "%s|%u|%u|%u|%d|%s|%s|%s|%u;", f->name,
                               f->link_type, f->connected, f->speed_mbps,
                               f->signal_dbm, f->ssid, f->ipv4, f->ipv6,
                               f->channel);
    }

    gboolean changed = gui_grid_signature_changed(s_grid, sig->str);

    g_string_free(sig, TRUE);

    if (!changed)
        return;

    gui_grid_clear(s_grid);

    if (snap->iface_count == 0) {
        gtk_grid_attach(GTK_GRID(s_grid),
                        gui_make_cell("未检测到网络接口", "detail-key"), 0, 0, 2,
                        1);
        return;
    }

    int row = 0;

    for (uint32_t i = 0; i < snap->iface_count && i < FMON_MAX_IFACES; i++) {
        const FmonIfaceInfo *f = &snap->ifaces[i];

        GString *line = g_string_new(NULL);

        g_string_append(line, iface_type_text(f->link_type));

        if (f->connected)
            g_string_append(line, " · 已连接");
        else
            g_string_append(line, " · 未连接");

        if (f->speed_mbps > 0)
            g_string_append_printf(line, " · %u Mbps", f->speed_mbps);

        if (f->ssid[0] != '\0')
            g_string_append_printf(line, " · SSID %s", f->ssid);

        if (f->channel > 0)
            g_string_append_printf(line, " · 信道 %u", f->channel);

        if (f->signal_dbm != 0)
            g_string_append_printf(line, " · 信号 %d dBm", f->signal_dbm);

        gtk_grid_attach(GTK_GRID(s_grid),
                        gui_make_cell(f->name, "detail-value"), 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(s_grid), gui_make_cell(line->str, NULL),
                        1, row, 1, 1);
        row++;

        GString *addr = g_string_new(NULL);

        g_string_append_printf(addr, "IPv4 %s",
                               (f->ipv4[0] != '\0') ? f->ipv4 : "—");
        g_string_append_printf(addr, "   IPv6 %s",
                               (f->ipv6[0] != '\0') ? f->ipv6 : "—");
        g_string_append_printf(addr, "   MAC %s",
                               (f->mac[0] != '\0') ? f->mac : "—");

        gtk_grid_attach(GTK_GRID(s_grid), gui_make_cell("", NULL), 0, row, 1,
                        1);
        gtk_grid_attach(GTK_GRID(s_grid),
                        gui_make_cell(addr->str, "detail-key"), 1, row, 1, 1);
        row++;

        g_string_free(line, TRUE);
        g_string_free(addr, TRUE);
    }

    if (snap->nic_count > 0) {
        GString *hw = g_string_new(NULL);

        for (uint32_t i = 0; i < snap->nic_count && i < 4; i++) {
            if (i > 0)
                g_string_append(hw, "\n");
            g_string_append(hw, snap->nic_models[i]);
        }

        gtk_grid_attach(GTK_GRID(s_grid),
                        gui_make_cell("网卡硬件", "detail-key"), 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(s_grid), gui_make_cell(hw->str, NULL), 1,
                        row, 1, 1);
        g_string_free(hw, TRUE);
    }
}

