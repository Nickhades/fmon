/*
 * kmem_stats.h - 内核内存（UMA 区）汇总
 *
 * 每 5 秒异步运行一次 `vmstat -z`，按区名归类：
 *   ZFS  （zfs_*）
 *   ZIO  （zio_*）
 *   页表 （vm_* / pv_* / *page_table*）
 * 以及全部区的合计。解析函数是纯函数，便于单测。
 * 运行在界面进程、非阻塞，不需要 root，也不改共享内存 ABI。
 */

#ifndef KMEM_STATS_H
#define KMEM_STATS_H

#include <glib.h>

typedef struct {
    guint64 total_bytes;   /* 所有 UMA 区合计（used × size） */
    guint64 zfs_bytes;
    guint64 zio_bytes;
    guint64 page_bytes;    /* 页表/页结构相关区 */
    guint   zone_count;    /* 解析到的区数量 */
} KmemSummary;

/* 解析 vmstat -z 文本；成功返回 TRUE。 */
gboolean kmem_stats_parse(const char *text, KmemSummary *out);

/* 5 秒节流：到期才真正启动一次异步采样。 */
void kmem_stats_update(void);

/* 最近一次采样；尚无数据返回 NULL。 */
const KmemSummary *kmem_stats_get(void);

#endif /* KMEM_STATS_H */
