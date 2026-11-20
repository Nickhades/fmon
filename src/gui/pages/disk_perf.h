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

    /* NVMe SMART 已移到 root daemon 采集，通过 FmonDiskInfo 发布。 */
} DiskPerf;

void disk_perf_update(void);
const DiskPerf *disk_perf_get(void);

/* 采集线程用：collect 只读 devstat，publish 在主线程发布。 */
void disk_perf_collect(DiskPerf *out);
void disk_perf_publish(const DiskPerf *in);

#endif /* DISK_PERF_H */
