/*
 * net_stats.h - 网络链路错误、丢包与 TCP 健康度
 *
 * 一次性读取 getifaddrs 的 struct if_data（所有接口的字节/错误/丢包），
 * 以及 net.inet.tcp.stats（重传、监听队列溢出）。不需要 root，1 Hz 采样，
 * 不占用共享内存 ABI。
 */

#ifndef NET_STATS_H
#define NET_STATS_H

#include <glib.h>

#define NET_STATS_MAX_IFS 16

typedef struct {
    char     name[32];
    char     addr[64];      /* 第一个 IPv4/IPv6 地址，空串表示没有 */
    guint    mtu;
    gboolean up;

    /* 无线接口（wlan*）的附加信息；非无线时 ssid 为空 */
    gboolean wireless;
    char     ssid[64];
    int      channel;
    char     media[96];
    guint64  ibytes, obytes;
    guint64  ierrors, oerrors;
    guint64  iqdrops;
} NetIfaceStat;

typedef struct {
    int          nifs;
    NetIfaceStat ifs[NET_STATS_MAX_IFS];

    /* TCP 累计计数与派生重传率 */
    guint64 tcp_sndtotal, tcp_rexmit, tcp_listendrop;
    guint64 tcp_connattempt, tcp_closed;
    double  retrans_pct;
} NetStats;

void net_stats_update(void);
const NetStats *net_stats_get(void);

/* 采集线程用：collect_raw 只读系统信息，publish 在主线程合并无线并发布。 */
void net_stats_collect_raw(NetStats *out);
void net_stats_publish(const NetStats *in);

/* 纯函数：百分比（无分母返回 0），便于单测 */
double net_stats_pct(guint64 part, guint64 total);

/* 纯函数：解析 ifconfig 输出里的无线信息（ssid/channel/media）。
 * 只填充 wlan* 接口；返回解析到的条目数。 */
int net_stats_parse_ifconfig(const char *text, NetStats *out);

#endif /* NET_STATS_H */
