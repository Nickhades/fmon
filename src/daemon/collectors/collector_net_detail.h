/* collector_net_detail.h - 网络接口明细 */
#ifndef FMON_COLLECTOR_NET_DETAIL_H
#define FMON_COLLECTOR_NET_DETAIL_H

#include <stdint.h>

typedef struct {
    char     name[16];
    char     ssid[36];
    uint32_t channel;
    int32_t  signal_dbm;
    uint32_t speed_mbps;
    uint32_t valid;
    int64_t  updated_ms;
} IfaceDetail;

void net_refresh_iface_detail(IfaceDetail *d, const char *name, int64_t now);

#endif
