/*
 * collector_net.c - 网卡吞吐、地址与接口明细
 *
 * 采集内容：
 *   - 每张网卡的累计/瞬时吞吐、错误计数、MTU、MAC；
 *   - IPv4 / IPv6 首选地址；
 *   - 接口类型（以太网 / Wi-Fi / 回环 / 虚拟）；
 *   - Wi-Fi：net80211 ioctl 读取 SSID/信道/RSSI/速率；失败才回退 ifconfig；
 *   - 有线：链路速率（从 ifconfig media 行解析，作为基础工具回退）。
 *
 * 子进程（ifconfig/pciconf）在共享内存写锁之外执行，明细每 5 秒刷新一次，
 * 避免每秒 fork 拖慢发布线程。
 */

#include "fmon_daemon.h"
#include "collector_net_detail.h"
#include "fmon_sysctl.h"

#include "fmon_child.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <net80211/ieee80211.h>
#include <net80211/ieee80211_ioctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char     name[16];
    uint64_t rx;
    uint64_t tx;
} IfacePrev;

static IfaceDetail g_detail[FMON_MAX_IFACES];

static int iface_find(const FmonIfaceInfo *arr, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].name, name) == 0)
            return i;
    }
    return -1;
}

static IfaceDetail *detail_slot(const char *name)
{
    for (int i = 0; i < FMON_MAX_IFACES; i++) {
        if (g_detail[i].name[0] != '\0' &&
            strcmp(g_detail[i].name, name) == 0)
            return &g_detail[i];
    }

    for (int i = 0; i < FMON_MAX_IFACES; i++) {
        if (g_detail[i].name[0] == '\0') {
            strlcpy(g_detail[i].name, name, sizeof(g_detail[i].name));
            return &g_detail[i];
        }
    }

    return NULL;
}

/* 从 "media: Ethernet autoselect (1000baseT <full-duplex>)" 提取速率 */
static void probe_nic_models(char models[4][96], uint32_t *count)
{
    *count = 0;

    const char *const argv[] = { "pciconf", "-lv", NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 128 * 1024, 3000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return;
    }

    char  *copy = strdup(res.out);
    char  *save = NULL;
    bool   want = false;
    bool   wifi = false;
    char   vendor[64] = {0};

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        if (strchr(line, '@') != NULL && strstr(line, "@pci") != NULL) {
            want = false;
            wifi = false;
        }

        /* 0x0200 = Ethernet，0x0280 = Network other（Wi-Fi 常归此类）。 */
        if (strstr(line, "class=0x0280") != NULL) {
            want = true;
            wifi = true;
        } else if (strstr(line, "class=0x0200") != NULL) {
            want = true;
            wifi = false;
        }

        if (!want)
            continue;

        char *v = strstr(line, "vendor     = '");
        if (v != NULL) {
            v += strlen("vendor     = '");
            char *end = strchr(v, '\'');

            if (end != NULL) {
                size_t len = (size_t)(end - v);

                if (len < sizeof(vendor)) {
                    memcpy(vendor, v, len);
                    vendor[len] = '\0';
                }
            }
        }

        char *d = strstr(line, "device     = '");
        if (d != NULL) {
            d += strlen("device     = '");
            char *end = strchr(d, '\'');

            if (end != NULL && *count < 4) {
                size_t len = (size_t)(end - d);

                if (len < 88) {
                    char name[96];
                    char devname[96];
                    size_t ncopy = (len < sizeof(devname) - 1)
                                       ? len
                                       : sizeof(devname) - 1;

                    memcpy(devname, d, ncopy);
                    devname[ncopy] = '\0';

                    snprintf(name, sizeof(name), "%s: %s%s%s",
                             wifi ? "Wi-Fi" : "Ethernet",
                             (vendor[0] != '\0') ? vendor : "",
                             (vendor[0] != '\0') ? " " : "", devname);

                    bool dup = false;

                    for (uint32_t k = 0; k < *count; k++) {
                        if (strcmp(models[k], name) == 0)
                            dup = true;
                    }

                    if (!dup)
                        strlcpy(models[(*count)++], name, 96);
                }
            }
        }
    }

    free(copy);
    fmon_child_result_free(&res);
}

/* 先算出结果，再在锁内一次性写入共享内存 */
static void sample_interfaces(FmonIfaceInfo *out, int *out_n, double *out_rx,
                              double *out_tx)
{
    static IfacePrev prev[FMON_MAX_IFACES];
    static int       prev_n  = 0;
    static int64_t   prev_ms = 0;

    struct ifaddrs *ifap = NULL;

    if (getifaddrs(&ifap) != 0)
        return;

    FmonIfaceInfo tmp[FMON_MAX_IFACES];
    int           n = 0;

    memset(tmp, 0, sizeof(tmp));

    /* 第一遍：链路层统计（只有 AF_LINK 条目带 struct if_data） */
    for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_LINK)
            continue;
        if (ifa->ifa_data == NULL)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        const struct if_data *ifd = ifa->ifa_data;

        int slot = iface_find(tmp, n, ifa->ifa_name);
        if (slot < 0) {
            if (n >= FMON_MAX_IFACES)
                continue;
            slot = n++;
            strlcpy(tmp[slot].name, ifa->ifa_name, sizeof(tmp[slot].name));
        }

        tmp[slot].mtu       = ifd->ifi_mtu;
        tmp[slot].flags     = ifa->ifa_flags;
        tmp[slot].rx_bytes  = ifd->ifi_ibytes;
        tmp[slot].tx_bytes  = ifd->ifi_obytes;
        tmp[slot].rx_errors = ifd->ifi_ierrors;
        tmp[slot].tx_errors = ifd->ifi_oerrors;

        const struct sockaddr_dl *sdl =
            (const struct sockaddr_dl *)ifa->ifa_addr;

        if (sdl->sdl_alen == 6) {
            const unsigned char *m = (const unsigned char *)LLADDR(sdl);

            snprintf(tmp[slot].mac, sizeof(tmp[slot].mac),
                       "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2],
                       m[3], m[4], m[5]);
        }
    }

    /* 第二遍：IPv4 / IPv6 首选地址 */
    for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        int fam = ifa->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6)
            continue;

        int slot = iface_find(tmp, n, ifa->ifa_name);
        if (slot < 0)
            continue;

        const void *src = (fam == AF_INET)
                              ? (const void *)&((const struct sockaddr_in *)
                                                    ifa->ifa_addr)->sin_addr
                              : (const void *)&((const struct sockaddr_in6 *)
                                                    ifa->ifa_addr)->sin6_addr;

        char *dst = (fam == AF_INET) ? tmp[slot].ipv4 : tmp[slot].ipv6;
        size_t len = (fam == AF_INET) ? sizeof(tmp[slot].ipv4)
                                      : sizeof(tmp[slot].ipv6);

        if (dst[0] == '\0')
            (void)inet_ntop(fam, src, dst, (socklen_t)len);

        if (tmp[slot].addr[0] == '\0')
            (void)inet_ntop(fam, src, tmp[slot].addr,
                            (socklen_t)sizeof(tmp[slot].addr));
    }

    freeifaddrs(ifap);

    /* 类型 + 明细 */
    int64_t now = fmon_now_ms();

    for (int i = 0; i < n; i++) {
        const char *nm = tmp[i].name;

        if (strncmp(nm, "lo", 2) == 0)
            tmp[i].link_type = FMON_LINK_LOOP;
        else if (strncmp(nm, "wlan", 4) == 0)
            tmp[i].link_type = FMON_LINK_WIFI;
        else if (strncmp(nm, "bridge", 6) == 0 ||
                 strncmp(nm, "epair", 5) == 0 ||
                 strncmp(nm, "vlan", 4) == 0 ||
                 strncmp(nm, "lagg", 4) == 0 ||
                 strncmp(nm, "tap", 3) == 0 ||
                 strncmp(nm, "tun", 3) == 0 ||
                 strncmp(nm, "vmnet", 5) == 0)
            tmp[i].link_type = FMON_LINK_VIRTUAL;
        else
            tmp[i].link_type = FMON_LINK_ETHER;

        IfaceDetail *d = detail_slot(nm);

        if (d != NULL && (tmp[i].flags & IFF_UP) != 0 &&
            tmp[i].link_type != FMON_LINK_LOOP)
            net_refresh_iface_detail(d, nm, now);

        if (d != NULL) {
            strlcpy(tmp[i].ssid, d->ssid, sizeof(tmp[i].ssid));
            tmp[i].channel      = d->channel;
            tmp[i].signal_dbm   = d->signal_dbm;
            tmp[i].speed_mbps   = d->speed_mbps;
            tmp[i].detail_valid = d->valid;
        }

        tmp[i].connected = ((tmp[i].flags & IFF_UP) != 0 &&
                            (tmp[i].ipv4[0] != '\0' ||
                             tmp[i].ipv6[0] != '\0'))
                               ? 1u
                               : 0u;
    }

    /* 用真实经过时间计算速率 */
    double dt = (prev_ms > 0) ? (double)(now - prev_ms) / 1000.0 : 0.0;

    double sum_rx = 0.0, sum_tx = 0.0;

    for (int i = 0; i < n; i++) {
        uint64_t prx = 0, ptx = 0;

        for (int j = 0; j < prev_n; j++) {
            if (strcmp(prev[j].name, tmp[i].name) == 0) {
                prx = prev[j].rx;
                ptx = prev[j].tx;
                break;
            }
        }

        if (dt > 0.05) {
            if (tmp[i].rx_bytes >= prx)
                tmp[i].rx_kbps = (double)(tmp[i].rx_bytes - prx) / 1024.0 / dt;
            if (tmp[i].tx_bytes >= ptx)
                tmp[i].tx_kbps = (double)(tmp[i].tx_bytes - ptx) / 1024.0 / dt;
        }

        if ((tmp[i].flags & IFF_UP) != 0) {
            sum_rx += tmp[i].rx_kbps;
            sum_tx += tmp[i].tx_kbps;
        }
    }

    /* 记录本次累计值作为下一轮的基准 */
    prev_n = (n < FMON_MAX_IFACES) ? n : FMON_MAX_IFACES;
    for (int i = 0; i < prev_n; i++) {
        strlcpy(prev[i].name, tmp[i].name, sizeof(prev[i].name));
        prev[i].rx = tmp[i].rx_bytes;
        prev[i].tx = tmp[i].tx_bytes;
    }
    prev_ms = now;

    memcpy(out, tmp, sizeof(tmp));
    *out_n = n;
    *out_rx = sum_rx;
    *out_tx = sum_tx;
}

void *thread_net(void *arg)
{
    (void)arg;

    FmonIfaceInfo ifaces[FMON_MAX_IFACES];

    char     models[4][96];
    uint32_t model_count = 0;

    probe_nic_models(models, &model_count);

    while (g_running) {
        int    n  = 0;
        double rx = 0.0, tx = 0.0;

        /* 子进程在锁外执行 */
        sample_interfaces(ifaces, &n, &rx, &tx);

        fmon_shm_wr_begin();
        g_shm->net_rx_kbps = rx;
        g_shm->net_tx_kbps = tx;
        g_shm->iface_count = (uint32_t)n;
        g_shm->nic_count   = model_count;
        memcpy(g_shm->nic_models, models, sizeof(models));
        memcpy(g_shm->ifaces, ifaces, sizeof(ifaces));
        fmon_shm_wr_end();

        if (!fmon_sleep_ms(FMON_SAMPLE_INTERVAL_MS) && !g_running)
            break;
    }

    return NULL;
}
