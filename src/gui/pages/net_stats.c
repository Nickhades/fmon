/*
 * net_stats.c - 网络链路/TCP 统计采样
 */

#include "net_stats.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp_var.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

static NetStats g_stats;
static gboolean g_net_running;   /* 工作线程采集进行中 */
static gboolean g_valid = FALSE;

double net_stats_pct(guint64 part, guint64 total)
{
    if (total == 0)
        return 0.0;

    double p = 100.0 * (double)part / (double)total;

    return (p < 0.0) ? 0.0 : ((p > 100.0) ? 100.0 : p);
}

static void read_tcp(NetStats *s)
{
    struct tcpstat ts;
    size_t         len = sizeof(ts);

    if (sysctlbyname("net.inet.tcp.stats", &ts, &len, NULL, 0) != 0)
        return;

    s->tcp_sndtotal   = ts.tcps_sndtotal;
    s->tcp_rexmit     = ts.tcps_sndrexmitpack;
    s->tcp_listendrop = ts.tcps_listendrop;
    s->tcp_connattempt = ts.tcps_connattempt;
    s->tcp_closed     = ts.tcps_closed;
    s->retrans_pct    = net_stats_pct(s->tcp_rexmit, s->tcp_sndtotal);
}

/* 无线接口详情由 daemon 通过 net80211 ioctl 采集后写入共享内存；
 * 本文件不再启动 ifconfig 子进程。parse_ifconfig 仅保留为纯解析单测。 */
int net_stats_parse_ifconfig(const char *text, NetStats *out)
{
    if (text == NULL || out == NULL)
        return 0;

    int   found = 0;
    char  cur[32] = "";
    char **lines = g_strsplit(text, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];

        if (line[0] != '\0' && line[0] != ' ' && line[0] != '\t') {
            const char *colon = strchr(line, ':');

            cur[0] = '\0';
            if (colon != NULL) {
                size_t n = (size_t)(colon - line);

                if (n >= sizeof(cur))
                    n = sizeof(cur) - 1;
                memcpy(cur, line, n);
                cur[n] = '\0';
            }
            continue;
        }

        if (!g_str_has_prefix(cur, "wlan"))
            continue;

        NetIfaceStat *st = NULL;

        for (int k = 0; k < out->nifs; k++) {
            if (g_strcmp0(out->ifs[k].name, cur) == 0) {
                st = &out->ifs[k];
                break;
            }
        }
        if (st == NULL)
            continue;

        const char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;

        if (g_str_has_prefix(p, "ssid ")) {
            const char *v = p + 5;
            const char *ch = strstr(v, " channel ");

            size_t n = (ch != NULL) ? (size_t)(ch - v) : strlen(v);

            if (n >= sizeof(st->ssid))
                n = sizeof(st->ssid) - 1;
            memcpy(st->ssid, v, n);
            st->ssid[n] = '\0';

            if (ch != NULL)
                st->channel = atoi(ch + 9);

            if (st->ssid[0] != '\0')
                found++;
        } else if (g_str_has_prefix(p, "media:")) {
            g_strlcpy(st->media, p + 6, sizeof(st->media));
            g_strstrip(st->media);
        }
    }

    g_strfreev(lines);
    return found;
}

void net_stats_collect_raw(NetStats *s)
{
    memset(s, 0, sizeof(*s));

    struct ifaddrs *ifa0 = NULL;

    if (getifaddrs(&ifa0) == 0) {
        /* 先收集接口地址（AF_INET/AF_INET6 的 ifa_data 不是 if_data） */
        GHashTable *addr_map = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, g_free);

        /* 优先 IPv4（lo0 要显示 127.0.0.1 而不是 ::1），
         * 没有再退回 IPv6，避免把回环显示成 ::1 让人困惑。 */
        for (int pass = 0; pass < 2; pass++) {
            int fam = (pass == 0) ? AF_INET : AF_INET6;

            for (struct ifaddrs *ifa = ifa0; ifa != NULL;
                 ifa = ifa->ifa_next) {
                if (ifa->ifa_addr == NULL || ifa->ifa_name == NULL)
                    continue;
                if (ifa->ifa_addr->sa_family != fam)
                    continue;
                if (g_hash_table_contains(addr_map, ifa->ifa_name))
                    continue;

                char buf[64] = "";
                void *src = (fam == AF_INET)
                                ? (void *)&((struct sockaddr_in *)
                                                ifa->ifa_addr)
                                      ->sin_addr
                                : (void *)&((struct sockaddr_in6 *)
                                                ifa->ifa_addr)
                                      ->sin6_addr;

                if (inet_ntop(fam, src, buf, sizeof(buf)) != NULL)
                    g_hash_table_insert(addr_map, g_strdup(ifa->ifa_name),
                                        g_strdup(buf));
            }
        }

        for (struct ifaddrs *ifa = ifa0; ifa != NULL;
             ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL ||
                ifa->ifa_addr->sa_family != AF_LINK ||
                ifa->ifa_data == NULL)
                continue;

            if (s->nifs >= NET_STATS_MAX_IFS)
                break;

            /* 同一接口的多个地址可能重复出现，按名字去重 */
            gboolean seen = FALSE;

            for (int i = 0; i < s->nifs; i++) {
                if (g_strcmp0(s->ifs[i].name, ifa->ifa_name) == 0) {
                    seen = TRUE;
                    break;
                }
            }
            if (seen)
                continue;

            const struct if_data *d = ifa->ifa_data;
            NetIfaceStat *st = &s->ifs[s->nifs++];

            g_strlcpy(st->name, ifa->ifa_name, sizeof(st->name));
            /* 用链路状态判断“有没有载波”：re0 管理上 UP 但没插网线时
             * IFF_RUNNING 仍可能置位，ifi_link_state 才是真实连接状态。 */
            st->up = (d->ifi_link_state == LINK_STATE_UP);
            st->mtu = d->ifi_mtu;

            const char *addr = g_hash_table_lookup(addr_map, ifa->ifa_name);

            if (addr != NULL)
                g_strlcpy(st->addr, addr, sizeof(st->addr));

            st->ibytes   = d->ifi_ibytes;
            st->obytes   = d->ifi_obytes;
            st->ierrors  = d->ifi_ierrors;
            st->oerrors  = d->ifi_oerrors;
            st->iqdrops  = d->ifi_iqdrops;
        }

        g_hash_table_destroy(addr_map);
        freeifaddrs(ifa0);
    }

    read_tcp(s);
}

static void net_task_thread(GTask *task, gpointer src, gpointer data,
                            GCancellable *cancellable)
{
    (void)src;
    (void)data;
    (void)cancellable;

    NetStats *s = g_new0(NetStats, 1);

    net_stats_collect_raw(s);
    g_task_return_pointer(task, s, g_free);
}

void net_stats_publish(const NetStats *in)
{
    if (in == NULL)
        return;

    NetStats s = *in;

    for (int i = 0; i < s.nifs; i++)
        s.ifs[i].wireless = g_str_has_prefix(s.ifs[i].name, "wlan");

    g_stats = s;
    g_valid = s.nifs > 0;
}

static void net_task_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    (void)data;

    GTask    *task = G_TASK(res);
    NetStats *s    = g_task_propagate_pointer(task, NULL);

    net_stats_publish(s);
    g_free(s);
    g_net_running = FALSE;
}

void net_stats_update(void)
{
    if (g_net_running)
        return;

    g_net_running = TRUE;

    GTask *task = g_task_new(NULL, NULL, net_task_done, NULL);

    g_task_run_in_thread(task, net_task_thread);
    g_object_unref(task);
}

const NetStats *net_stats_get(void)
{
    return g_valid ? &g_stats : NULL;
}
