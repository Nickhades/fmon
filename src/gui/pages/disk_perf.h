/*
 * disk_perf.h - 磁盘实时性能（IOPS / 延迟 / 繁忙 / 队列）
 *
 * 用 devstat(3) 读取内核磁盘统计，按秒差分得到：
 *   读/写 IOPS、平均响应延迟（ms）、设备繁忙度（%）、队列长度。
 * 需要链接 libdevstat；不需要 root（与 gstat 同源）。
 */

#ifndef DISK_PERF_H
#define DISK_PERF_H

#include <glib.h>

typedef struct {
    gboolean valid;
    char     dev[24];
    double   read_iops;
    double   write_iops;
    double   latency_ms;
    double   busy_pct;
    double   queue_len;

    /* 累计字节（devstat，绝对量） */
    guint64  total_read_bytes;
    guint64  total_write_bytes;

    /* NVMe SMART（nvmecontrol，30 秒刷新；不可用时 has_smart = FALSE） */
    gboolean has_smart;
    double   temp_c;
    int      spare_pct;    /* Available spare */
    int      wear_pct;     /* Percentage used */
} DiskPerf;

void disk_perf_update(void);
const DiskPerf *disk_perf_get(void);

/* 采集线程用：collect 只读 devstat，publish 在主线程发布并触发 SMART。 */
void disk_perf_collect(DiskPerf *out, char smart_dev[16]);
void disk_perf_publish(const DiskPerf *in, const char *smart_dev);

/* 纯函数：解析 nvmecontrol logpage -p 2 的 SMART 文本 */
gboolean disk_perf_parse_smart(const char *text, double *temp_c,
                               int *spare_pct, int *wear_pct);

#endif /* DISK_PERF_H */
