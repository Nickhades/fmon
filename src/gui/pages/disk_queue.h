/*
 * disk_queue.h - 设备队列长度（iostat -x 的 qlen）与繁忙度
 */

#ifndef FMON_GUI_DISK_QUEUE_H
#define FMON_GUI_DISK_QUEUE_H

#include <glib.h>

typedef struct {
    char   name[16];
    double qlen;      /* 当前队列长度 */
    double busy_pct;  /* %b 设备繁忙百分比 */
} DiskQueueDev;

typedef struct {
    int      n;
    DiskQueueDev dev[8];
} DiskQueue;

/* 纯函数，便于单测：解析 iostat -x 输出 */
gboolean disk_queue_parse(const char *text, DiskQueue *out);

/* 异步刷新（5 秒节流） */
void disk_queue_update(void);

const DiskQueue *disk_queue_get(void);

#endif /* FMON_GUI_DISK_QUEUE_H */
