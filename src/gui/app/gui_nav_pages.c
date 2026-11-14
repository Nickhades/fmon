/*
 * gui_nav_pages.c - 导航页面与分组数据表
 */

#include "gui_nav_pages.h"

#include <glib.h>

#include <string.h>

typedef struct {
    const char *id;
    const char *title;
    const char *icon;
    const char *desc;
} PageMeta;

typedef struct {
    const char *title;
    const char *icon;
    const char *desc;
    int         pages[12];
    int         n_pages;
} NavGroup;

static const GuiNavPage g_pages[] = {
    { "cpu",     "CPU",        "fmon-cpu-symbolic",                      "使用率、负载与温度" },
    { "mem",     "内存",       "fmon-memory-symbolic", "容量与使用率" },
    { "swap",    "Swap",       "fmon-swap-symbolic",           "交换区使用" },
    { "netm",    "网络概览",   "fmon-network-symbolic",            "收发速率、链路错误与 TCP 健康度" },
    { "disk",    "磁盘",       "fmon-disk-symbolic",          "读写速率与 IOPS" },
    { "gpu",     "GPU",        "fmon-gpu-symbolic",            "负载、显存与温度" },
    { "history", "历史记录",   "fmon-history-symbolic",     "最近 1 小时 / 24 小时曲线" },
    { "zpool",   "ZFS 池空间", "fmon-zfs-symbolic",           "池容量与使用率" },
    { "arc",     "ZFS ARC",    "fmon-zfs-symbolic",           "ARC 大小与占比" },
    { "zhit",    "ZFS 命中率", "fmon-zfs-symbolic",           "命中率与幽灵命中" },
    { "zread",   "zpool 读带宽", "fmon-zfs-symbolic",        "读速率与累计" },
    { "zwrite",  "zpool 写带宽", "fmon-zfs-symbolic",        "写速率与累计" },
    { "zprune",  "ARC Prune",  "fmon-zfs-symbolic",           "ARC 回收次数" },
    { "temp",    "CPU 温度",   "fmon-temperature-symbolic",                      "温度与主机信息" },
    { "batt",    "电池",       "fmon-battery-symbolic",                  "电量、功率与电源" },
    { "storage", "存储空间",   "fmon-storage-symbolic",          "挂载点容量与磁盘速率" },
    { "proc",    "进程管理",   "fmon-process-symbolic",          "查看、过滤与结束进程" },
    { "svc",     "服务管理",   "fmon-service-symbolic",               "服务启停与开机自启" },
    { "log",     "系统日志",   "fmon-log-symbolic",           "读取系统日志" },
    { "conn",    "活动连接",   "fmon-connection-symbolic",            "当前 TCP/UDP 连接" },
    { "alerts",  "告警历史",   "fmon-alerts-symbolic",           "告警记录与静默" },
    { "rctl",    "资源限制",   "fmon-limits-symbolic",          "rctl 资源规则管理" },
    { "iface",   "网络接口",   "fmon-interface-symbolic", "地址、状态与收发速率" },
    { "zsum",    "ZFS 总览",   "fmon-zfs-symbolic",           "池空间 / ARC / 命中率 / Prune" },
    { "zbw",     "ZFS 带宽",   "fmon-zfs-symbolic",          "读写带宽" },
    { "gpu0",    "GPU 0",      "fmon-gpu-symbolic",            "显卡明细" },
    { "gpu1",    "GPU 1",      "fmon-gpu-symbolic",            "显卡明细" },
    { "gpu2",    "GPU 2",      "fmon-gpu-symbolic",            "显卡明细" },
    { "gpu3",    "GPU 3",      "fmon-gpu-symbolic",            "显卡明细" },
    { "l2arc",   "L2ARC",      "fmon-zfs-symbolic",           "二级读缓存明细" },
};

static const GuiNavGroup g_groups[] = {
    { "概览",       "fmon-overview-symbolic", "按指标查看性能",
      { 0, 1, 25, 26, 27, 28, 6 }, 7 },
    { "存储",       "fmon-storage-symbolic",  "磁盘、ZFS 与挂载点",
      { 4, 7, 8, 9, 12, 24, 15, 29 }, 8 },
    { "系统",       "fmon-system-symbolic",   "进程、服务、日志与电池",
      { 16, 17, 18, 14 }, 4 },
    { "网络",       "fmon-network-symbolic",  "概览与连接",
      { 3, 19 }, 2 },
    { "告警与限制", "fmon-alerts-symbolic",   "告警与资源规则",
      { 20, 21 }, 2 },
};


int gui_nav_group_count(void)
{
    return (int)G_N_ELEMENTS(g_groups);
}

const GuiNavGroup *gui_nav_group(int gi)
{
    if (gi < 0 || gi >= (int)G_N_ELEMENTS(g_groups))
        return NULL;

    return &g_groups[gi];
}

int gui_nav_page_count(void)
{
    return (int)G_N_ELEMENTS(g_pages);
}

const GuiNavPage *gui_nav_page(int idx)
{
    if (idx < 0 || idx >= (int)G_N_ELEMENTS(g_pages))
        return NULL;

    return &g_pages[idx];
}

int gui_nav_find_page(const char *id)
{
    for (int i = 0; i < (int)G_N_ELEMENTS(g_pages); i++) {
        if (strcmp(g_pages[i].id, id) == 0)
            return i;
    }

    return -1;
}

int gui_nav_group_of_page(int idx)
{
    for (int g = 0; g < (int)G_N_ELEMENTS(g_groups); g++) {
        for (int k = 0; k < g_groups[g].n_pages; k++) {
            if (g_groups[g].pages[k] == idx)
                return g;
        }
    }

    return -1;
}
