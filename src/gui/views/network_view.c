/*
 * network_view.c - 网络连接 + 网络接口
 *
 * 旧版本的问题：
 *   1. sscanf("%31s %63s %d %*s %15s %127s %127s") 只检查 >= 5 个字段，
 *      当 sockstat 输出不足 7 列时，foreign 数组未初始化就被 strlcpy，
 *      属于读取未初始化栈内存（可能崩溃或泄露栈内容）。
 *   2. 只查 IPv4（sockstat -4），IPv6 连接完全看不到。
 *   3. 在 GTK 主线程里同步 popen()，连接多时界面会卡住。
 *   4. 没有搜索、没有自动刷新、看不到接口。
 *
 * 现在：按空白切分（列数固定且地址中不含空格），严格校验字段数；
 * 异步执行；增加过滤、自动刷新和“网络接口”标签页。
 */

#include <glib/gi18n.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <string.h>
#include <sys/socket.h>

#include "network_view.h"

#include "network_internal.h"

#include "gui_state.h"
#include "gui_util.h"

/* ===========================================================================
 * 连接条目
 * ========================================================================= */
static GHashTable *s_seen;   /* 连接键 -> 首次看到的时间（单调时钟） */

static void norm_addr(const char *in, char *out, size_t n)
{
    g_strlcpy(out, in, n);

    char *dot   = strrchr(out, '.');
    char *colon = strrchr(out, ':');

    if (dot != NULL && (colon == NULL || dot > colon))
        *dot = ':';
}

static gchar *conn_key(const char *proto, const char *local,
                       const char *foreign)
{
    char l[256], f[256];

    norm_addr(local, l, sizeof(l));
    norm_addr(foreign, f, sizeof(f));
    return g_strdup_printf("%s|%s|%s", proto, l, f);
}

static void addr_host(const char *addr, char *out, size_t n)
{
    g_strlcpy(out, addr, n);

    char *c = strrchr(out, ':');

    if (c != NULL)
        *c = '\0';
}

static void iface_for_addr(const char *local, char *out, size_t n)
{
    char host[128];

    addr_host(local, host, sizeof(host));
    g_strlcpy(out, "*", n);

    if (host[0] == '\0' || strcmp(host, "*") == 0)
        return;

    if (strncmp(host, "127.", 4) == 0 || strcmp(host, "::1") == 0) {
        g_strlcpy(out, "lo0", n);
        return;
    }

    struct ifaddrs *ifa = NULL;

    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p != NULL; p = p->ifa_next) {
            if (p->ifa_addr == NULL)
                continue;

            char buf[INET6_ADDRSTRLEN] = { 0 };

            if (p->ifa_addr->sa_family == AF_INET)
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)p->ifa_addr)->sin_addr, buf,
                          sizeof(buf));
            else if (p->ifa_addr->sa_family == AF_INET6)
                inet_ntop(AF_INET6,
                          &((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr, buf,
                          sizeof(buf));
            else
                continue;

            if (strcmp(buf, host) == 0) {
                g_strlcpy(out, p->ifa_name, n);
                break;
            }
        }

        freeifaddrs(ifa);
    }
}

static const char *service_for(const char *foreign, const char *proto,
                               char *buf, size_t n)
{
    const char *c = strrchr(foreign, ':');

    if (c == NULL)
        return "—";

    char *end  = NULL;
    long  port = strtol(c + 1, &end, 10);

    if (end == c + 1 || port <= 0 || port > 65535)
        return "—";

    const char *pname = (g_ascii_strncasecmp(proto, "udp", 3) == 0) ? "udp"
                                                                   : "tcp";
    struct servent *se =
        getservbyport(htons((unsigned short)port), pname);

    g_snprintf(buf, n, "%s", (se != NULL) ? se->s_name : "—");
    return buf;
}

static void parse_netstat_states(const char *text, GHashTable *states)
{
    gchar **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;

        gchar **tok = g_strsplit_set(lines[i], " \t", -1);
        GPtrArray *parts = g_ptr_array_new();

        for (int j = 0; tok[j] != NULL; j++)
            if (tok[j][0] != '\0')
                g_ptr_array_add(parts, tok[j]);

        if (parts->len >= 5) {
            const char *proto = g_ptr_array_index(parts, 0);

            if ((g_ascii_strncasecmp(proto, "tcp", 3) == 0 ||
                 g_ascii_strncasecmp(proto, "udp", 3) == 0) &&
                ((const char *)g_ptr_array_index(parts, 3))[0] != '\0') {
                const char *local   = g_ptr_array_index(parts, 3);
                const char *foreign = g_ptr_array_index(parts, 4);
                const char *state   = (parts->len >= 6)
                                          ? g_ptr_array_index(parts, 5)
                                          : "";
                gchar *k = conn_key(proto, local, foreign);

                g_hash_table_insert(states, k, g_strdup(state));
            }
        }

        g_ptr_array_free(parts, TRUE);
        g_strfreev(tok);
    }

    g_strfreev(lines);
}

static void fill_connections(GListStore *store, const gchar *output)
{
    g_list_store_remove_all(store);

    if (output == NULL)
        return;

    const char *marker = strstr(output, "__NETSTAT__");
    gsize sock_len = (marker != NULL) ? (gsize)(marker - output) : strlen(output);
    gchar *sock = g_strndup(output, sock_len);

    GHashTable *states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               g_free);

    if (marker != NULL)
        parse_netstat_states(marker + strlen("__NETSTAT__"), states);

    GHashTable *next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             g_free);
    gchar **lines = g_strsplit(sock, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        if (i == 0 || lines[i][0] == '\0')
            continue;

        gchar **tok = g_strsplit_set(lines[i], " \t", -1);
        GPtrArray *parts = g_ptr_array_new();

        for (int j = 0; tok[j] != NULL; j++)
            if (tok[j][0] != '\0')
                g_ptr_array_add(parts, tok[j]);

        const char *proto = (parts->len >= 7) ? g_ptr_array_index(parts, 4)
                                             : NULL;

        if (parts->len >= 7 && proto != NULL &&
            (g_ascii_strncasecmp(proto, "tcp", 3) == 0 ||
             g_ascii_strncasecmp(proto, "udp", 3) == 0)) {
            const char *pid_str = g_ptr_array_index(parts, 2);
            gchar      *end     = NULL;
            gulong      pid     = g_ascii_strtoull(pid_str, &end, 10);

            if (end != NULL && *end == '\0' && pid > 0 && pid <= G_MAXUINT32) {
                NetItem *item = g_object_new(net_item_get_type(), NULL);

                item->user    = g_strdup(g_ptr_array_index(parts, 0));
                item->command = g_utf8_make_valid(g_ptr_array_index(parts, 1), -1);
                item->pid     = (guint32)pid;
                item->proto   = g_strdup(proto);
                item->local   = g_strdup(g_ptr_array_index(parts, 5));
                item->foreign = g_strdup(g_ptr_array_index(parts, 6));

                item->fd = (guint32)g_ascii_strtoull(
                    g_ptr_array_index(parts, 3), NULL, 10);

                struct passwd *pw = getpwnam(item->user);

                item->uid = (pw != NULL) ? (guint32)pw->pw_uid : 0;

                gchar *k = conn_key(proto, item->local, item->foreign);
                const char *st = g_hash_table_lookup(states, k);

                g_free(k);
                item->state = g_strdup((st != NULL) ? st : "");

                char svc[64];

                item->service =
                    g_strdup(service_for(item->foreign, proto, svc,
                                         sizeof(svc)));

                char ifc[32];

                iface_for_addr(item->local, ifc, sizeof(ifc));
                item->iface = g_strdup(ifc);

                gchar *base = conn_key(proto, item->local, item->foreign);
                gchar *key  = g_strdup_printf("%s|%u|%u", base, item->pid,
                                              item->fd);
                gint64 *prev = (s_seen != NULL)
                                   ? g_hash_table_lookup(s_seen, key)
                                   : NULL;

                item->since_us = (prev != NULL) ? *prev : g_get_monotonic_time();
                g_hash_table_insert(next, g_strdup(key),
                                    g_memdup2(&item->since_us,
                                              sizeof(item->since_us)));
                g_free(base);
                g_free(key);

                g_list_store_append(store, item);
                g_object_unref(item);
            }
        }

        g_ptr_array_free(parts, TRUE);
        g_strfreev(tok);
    }

    g_strfreev(lines);
    g_free(sock);
    g_hash_table_destroy(states);

    if (s_seen != NULL)
        g_hash_table_destroy(s_seen);

    s_seen = next;
}

/* ===========================================================================
 * 连接刷新（异步）
 * ========================================================================= */
static void on_sockstat_done(gpointer user_data, const FmonChildResult *res,
                             gboolean ok)
{
    GListStore *store = G_LIST_STORE(user_data);

    if (ok && res != NULL) {
        gchar *text = fmon_gui_make_valid_utf8(res->out, (gssize)res->out_len);
        fill_connections(store, text);
        g_free(text);
    }

    g_object_unref(store);
}

static void refresh_connections_async(GListStore *store)
{
    /* sockstat 默认同时输出 IPv4 / IPv6 的 TCP 与 UDP */
    const char *argv[] = {
        "sh", "-c",
        "sockstat -4 -6 2>/dev/null; echo __NETSTAT__; netstat -an 2>/dev/null",
        NULL
    };

    g_object_ref(store);
    fmon_gui_run_async_full(argv, 2 * 1024 * 1024, 4000, on_sockstat_done,
                            store);
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    refresh_connections_async(G_LIST_STORE(data));
}

/* ===========================================================================
 * 过滤器 / 排序
 * ========================================================================= */
static gboolean conn_filter_func(gpointer item, gpointer user_data)
{
    NetItem   *n      = item;
    const char *needle = user_data;

    if (needle == NULL || needle[0] == '\0')
        return TRUE;

    gchar *hay = g_strdup_printf("%s %u %s %u %s %s %s %s %s %s %u", n->user,
                                 n->uid, n->command, n->pid, n->proto,
                                 n->state, n->service, n->local, n->foreign,
                                 n->iface, n->fd);
    gchar *lower_hay    = g_utf8_strdown(hay, -1);
    gchar *lower_needle = g_utf8_strdown(needle, -1);
    gboolean found      = (strstr(lower_hay, lower_needle) != NULL);

    g_free(hay);
    g_free(lower_hay);
    g_free(lower_needle);
    return found;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    GtkCustomFilter *filter = GTK_CUSTOM_FILTER(data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    gtk_custom_filter_set_filter_func(filter, conn_filter_func, g_strdup(text),
                                      g_free);
}

/* ===========================================================================
 * 接口刷新（直接读快照）
 * ========================================================================= */
static void rebuild_ifaces(GListStore *store)
{
    const FmonShmData *s = gui_state_snapshot();

    g_list_store_remove_all(store);

    uint32_t n = s->iface_count;
    if (n > FMON_MAX_IFACES)
        n = FMON_MAX_IFACES;

    for (uint32_t i = 0; i < n; i++) {
        const FmonIfaceInfo *ii = &s->ifaces[i];

        IfaceItem *item = g_object_new(iface_item_get_type(), NULL);
        item->name   = g_strdup(ii->name);
        item->addr   = g_strdup((ii->addr[0] != '\0') ? ii->addr : "—");
        item->mtu    = ii->mtu;
        item->rx     = ii->rx_kbps;
        item->tx     = ii->tx_kbps;
        item->errors = ii->rx_errors + ii->tx_errors;
        item->state  = g_strdup(((ii->flags & 0x1u) != 0) ? "UP" : "DOWN");

        g_list_store_append(store, item);
        g_object_unref(item);
    }
}

/* ===========================================================================
 * 定时器
 * ========================================================================= */
static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);
    if (id != 0)
        g_source_remove(id);
}

static GtkWidget *g_conn_view  = NULL;
static GtkWidget *g_iface_view = NULL;

static gboolean on_conn_tick(gpointer data)
{
    if (fmon_gui_main_window() == NULL)
        return G_SOURCE_REMOVE;

    /* 只在网络页面可见时刷新，避免后台空转 */
    if (g_conn_view == NULL || !gtk_widget_get_mapped(g_conn_view))
        return G_SOURCE_CONTINUE;

    refresh_connections_async(G_LIST_STORE(data));
    return G_SOURCE_CONTINUE;
}

static gboolean on_iface_tick(gpointer data)
{
    if (fmon_gui_main_window() == NULL)
        return G_SOURCE_REMOVE;

    if (g_iface_view == NULL || !gtk_widget_get_mapped(g_iface_view))
        return G_SOURCE_CONTINUE;

    if (gui_state_online())
        rebuild_ifaces(G_LIST_STORE(data));

    return G_SOURCE_CONTINUE;
}

/* ===========================================================================
 * 构建页面
 * ========================================================================= */
static GtkWidget *build_connections_tab(GListStore **out_store)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn = gtk_button_new_with_label(_("刷新连接"));
    gtk_widget_add_css_class(btn, "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), btn);

    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_set_hexpand(search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), _("按进程、PID、地址或协议过滤"));
    gtk_box_append(GTK_BOX(toolbar), search);

    gtk_box_append(GTK_BOX(vbox), toolbar);

    GListStore *store = g_list_store_new(net_item_get_type());
    *out_store = store;

    GtkCustomFilter *filter =
        gtk_custom_filter_new(conn_filter_func, g_strdup(""), g_free);
    GtkFilterListModel *filter_model =
        gtk_filter_list_model_new(G_LIST_MODEL(store), GTK_FILTER(filter));

    GtkSortListModel *sort_model =
        gtk_sort_list_model_new(G_LIST_MODEL(filter_model), NULL);
    GtkSingleSelection *selection =
        gtk_single_selection_new(G_LIST_MODEL(sort_model));
    gtk_single_selection_set_autoselect(selection, FALSE);

    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);

    network_conn_columns_attach(GTK_COLUMN_VIEW(view));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_refresh_clicked), store);
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed),
                     filter);

    return vbox;
}

static GtkWidget *build_ifaces_tab(GListStore **out_store)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *hint = gtk_label_new(_("接口数据来自 getifaddrs(2)，速率按真实采样间隔计算"));
    gtk_widget_add_css_class(hint, "small-text");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), hint);

    GListStore *store = g_list_store_new(iface_item_get_type());
    *out_store = store;

    GtkSingleSelection *selection =
        gtk_single_selection_new(G_LIST_MODEL(store));
    gtk_single_selection_set_autoselect(selection, FALSE);

    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);

    network_iface_columns_attach(GTK_COLUMN_VIEW(view));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    return vbox;
}

static GtkWidget *network_view_build(gboolean with_conn, gboolean with_iface)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(root, TRUE);
    gtk_widget_add_css_class(root, "view-card");

    if (with_conn) {
        GListStore *conn_store = NULL;
        GtkWidget  *conn_tab   = build_connections_tab(&conn_store);

        gtk_box_append(GTK_BOX(root), conn_tab);
        refresh_connections_async(conn_store);

        g_object_set_data_full(G_OBJECT(root), "conn-store",
                               g_object_ref(conn_store), g_object_unref);

        guint t = g_timeout_add_seconds(5, on_conn_tick, conn_store);
        g_object_set_data_full(G_OBJECT(root), "conn-timer",
                               GUINT_TO_POINTER(t), remove_source);

        g_conn_view = root;
    }

    if (with_iface) {
        GListStore *iface_store = NULL;
        GtkWidget  *iface_tab   = build_ifaces_tab(&iface_store);

        gtk_box_append(GTK_BOX(root), iface_tab);
        rebuild_ifaces(iface_store);

        g_object_set_data_full(G_OBJECT(root), "iface-store",
                               g_object_ref(iface_store), g_object_unref);

        guint t = g_timeout_add_seconds(2, on_iface_tick, iface_store);
        g_object_set_data_full(G_OBJECT(root), "iface-timer",
                               GUINT_TO_POINTER(t), remove_source);

        g_iface_view = root;
    }

    return root;
}

GtkWidget *gui_network_conn_view_new(void)
{
    return network_view_build(TRUE, FALSE);
}

GtkWidget *gui_network_iface_view_new(void)
{
    return network_view_build(FALSE, TRUE);
}
