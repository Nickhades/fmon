/* collector_net_detail.c - ifconfig/WiFi 接口明细 */
#include "collector_net_detail.h"

#include "fmon_child.h"
#include "fmon_sysctl.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <net80211/ieee80211.h>
#include <net80211/ieee80211_ioctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>

static uint32_t parse_media_speed(const char *line);
#include <string.h>
#include <unistd.h>

static void parse_ifconfig(const char *out, const char *name, IfaceDetail *d)
{
    char *copy = strdup(out);
    char *save = NULL;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        char *ssid = strstr(line, "ssid ");
        if (ssid != NULL && strncmp(line, "\tssid ", 6) == 0) {
            char *val = ssid + 5;
            char *end = strstr(val, " channel");
            size_t len = (end != NULL) ? (size_t)(end - val) : strlen(val);

            if (len > 0 && len < sizeof(d->ssid)) {
                memcpy(d->ssid, val, len);
                d->ssid[len] = '\0';
            }
            continue;
        }

        char *ch = strstr(line, "channel ");
        if (ch != NULL) {
            d->channel = (uint32_t)strtoul(ch + 8, NULL, 10);
            continue;
        }

        char *media = strstr(line, "media: Ethernet");
        if (media != NULL) {
            uint32_t sp = parse_media_speed(media);

            if (sp > 0)
                d->speed_mbps = sp;
        }
    }

    (void)name;
    free(copy);
}

/* 读取 Wi-Fi 关联站点表，取信号强度（dBm） */
static void parse_sta_signal(const char *out, IfaceDetail *d)
{
    char *copy = strdup(out);
    char *save = NULL;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        char mac[32] = {0};
        char rate[32] = {0};
        int  chan = 0;
        int  rssi = 0;

        if (sscanf(line, "%31s %d %31s %d", mac, &chan, rate, &rssi) >= 4) {
            d->signal_dbm = rssi;
            if (chan > 0)
                d->channel = (uint32_t)chan;
            break;
        }
    }

    free(copy);
}

/* ---------------------------------------------------------------------------
 * Wi-Fi：直接走 net80211 ioctl（SIOCG80211），不再依赖 ifconfig 文本。
 * 失败时上层仍可回退 ifconfig（base 工具）。
 * ------------------------------------------------------------------------- */
static int wifi_req(int s, const char *name, int type, void *data, size_t len,
                    int *out_len)
{
    struct ieee80211req ireq;

    memset(&ireq, 0, sizeof(ireq));
    strlcpy(ireq.i_name, name, sizeof(ireq.i_name));
    ireq.i_type = (uint16_t)type;
    ireq.i_data = data;
    ireq.i_len  = (uint16_t)len;

    if (ioctl(s, SIOCG80211, &ireq) < 0)
        return -1;

    if (out_len != NULL)
        *out_len = (int)ireq.i_len;

    return 0;
}

static int wifi_req_val(int s, const char *name, int type, int *val)
{
    struct ieee80211req ireq;

    memset(&ireq, 0, sizeof(ireq));
    strlcpy(ireq.i_name, name, sizeof(ireq.i_name));
    ireq.i_type = (uint16_t)type;

    if (ioctl(s, SIOCG80211, &ireq) < 0)
        return -1;

    *val = ireq.i_val;
    return 0;
}

static bool wifi_ioctl_detail(const char *name, IfaceDetail *d)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0)
        return false;

    bool got = false;
    char ssid[IEEE80211_NWID_LEN + 1];
    int  slen = 0;

    memset(ssid, 0, sizeof(ssid));

    if (wifi_req(s, name, IEEE80211_IOC_SSID, ssid, sizeof(ssid) - 1,
                 &slen) == 0 && slen > 0) {
        if (slen > (int)sizeof(ssid) - 1)
            slen = (int)sizeof(ssid) - 1;
        ssid[slen] = '\0';
        strlcpy(d->ssid, ssid, sizeof(d->ssid));
        got = true;
    }

    struct ieee80211_channel chan;
    int                      clen = 0;

    memset(&chan, 0, sizeof(chan));

    if (wifi_req(s, name, IEEE80211_IOC_CURCHAN, &chan, sizeof(chan),
                 &clen) == 0 && clen >= (int)sizeof(chan)) {
        if (chan.ic_ieee != 0) {
            d->channel = chan.ic_ieee;
            got = true;
        }
    } else {
        int v = 0;

        if (wifi_req_val(s, name, IEEE80211_IOC_CHANNEL, &v) == 0 && v > 0) {
            d->channel = (uint32_t)v;
            got = true;
        }
    }

    uint8_t bssid[IEEE80211_ADDR_LEN];
    bool    have_bssid = false;

    memset(bssid, 0, sizeof(bssid));

    static const uint8_t zero_bssid[IEEE80211_ADDR_LEN] = { 0 };

    if (wifi_req(s, name, IEEE80211_IOC_BSSID, bssid, sizeof(bssid),
                 NULL) == 0 && memcmp(bssid, zero_bssid, sizeof(bssid)) != 0)
        have_bssid = true;

    union {
        struct ieee80211req_sta_req req;
        uint8_t                     buf[24 * 1024];
    } u;

    memset(&u, 0, sizeof(u));

    if (have_bssid) {
        memcpy(u.req.is_u.macaddr, bssid, sizeof(bssid));
    } else {
        memset(u.req.is_u.macaddr, 0xff, sizeof(u.req.is_u.macaddr));
    }

    int len = 0;

    if (wifi_req(s, name, IEEE80211_IOC_STA_INFO, &u, sizeof(u), &len) == 0 &&
        len >= (int)sizeof(struct ieee80211req_sta_info)) {
        const struct ieee80211req_sta_info *si =
            (const struct ieee80211req_sta_info *)(const void *)u.req.info;

        d->signal_dbm = si->isi_rssi / 2;
        if (si->isi_txmbps > 0)
            d->speed_mbps = (uint32_t)(si->isi_txmbps / 2);
        got = true;
    }

    close(s);
    return got;
}

static uint32_t parse_media_speed(const char *line)
{
    const char *base = strstr(line, "base");
    if (base == NULL)
        return 0;
    const char *p = base;
    while (p > line && (p[-1] >= '0' && p[-1] <= '9'))
        p--;
    if (p == base)
        return 0;
    long v = strtol(p, NULL, 10);
    if (v < 0 || v > 400000)
        return 0;
    return (uint32_t)v;
}


void net_refresh_iface_detail(IfaceDetail *d, const char *name, int64_t now)
{
    if (d == NULL)
        return;

    if (d->updated_ms != 0 && now - d->updated_ms < 5000)
        return;

    d->updated_ms = now;
    d->channel    = 0;
    d->signal_dbm = 0;
    d->speed_mbps = 0;
    d->valid      = 0;
    d->ssid[0]    = '\0';

    /* Wi-Fi 优先：net80211 ioctl，避免解析 ifconfig 文本。 */
    if (strncmp(name, "wlan", 4) == 0 && wifi_ioctl_detail(name, d)) {
        d->valid = 1;
        return;
    }

    /* 回退：ifconfig（base 工具）；Wi-Fi 仍需读取关联站点 RSSI。 */
    const char *const argv[] = { "ifconfig", name, NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 32 * 1024, 1500, &res) == 0 &&
        fmon_child_ok(&res) && res.out != NULL) {
        parse_ifconfig(res.out, name, d);
        d->valid = 1;
    }

    fmon_child_result_free(&res);

    if (strncmp(name, "wlan", 4) == 0) {
        const char *const sta[] = { "ifconfig", name, "list", "sta", NULL };

        FmonChildResult sres;

        if (fmon_child_run(sta, 32 * 1024, 1500, &sres) == 0 &&
            fmon_child_ok(&sres) && sres.out != NULL)
            parse_sta_signal(sres.out, d);

        fmon_child_result_free(&sres);
    }
}


/* 用 pciconf 读取网卡芯片型号（只需要在启动后做一次） */
