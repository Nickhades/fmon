/*
 * vm_stats.h - FreeBSD 虚拟内存队列明细（GUI 直接读 sysctl）
 *
 * 共享内存（ABI 19）只带 total/free/avail，画不出 Active/Inactive/Laundry
 * 这些队列的构成。这些 vm.stats.vm.* 计数器是全局只读、无需 root，
 * 且页面本来就是 1 Hz 刷新，所以这里由 GUI 自己采样，避免为此升 ABI
 * 并强制下游重装 daemon。
 */

#ifndef VM_STATS_H
#define VM_STATS_H

#include <glib.h>

typedef struct {
    guint64 page_size;
    guint64 total;        /* v_page_count * page_size */
    guint64 active;       /* v_active_count   */
    guint64 inactive;     /* v_inactive_count */
    guint64 laundry;      /* v_laundry_count  */
    guint64 wired;        /* v_wire_count（含 ZFS ARC） */
    guint64 free;         /* v_free_count     */

    /* ZFS ARC 大小：FreeBSD 的 cache 队列（v_cache_count）现在基本恒为 0，
     * 真正的“缓存”是 ARC，而 ARC 计在 Wired 里。由调用方从快照的
     * zfs_arc_size 填入；无 ZFS 时为 0。 */
    guint64 arc;

    guint64 bufspace;     /* vfs.bufspace：缓冲缓存（top 的 Buf），唯一的
                           * 显式“缓存”计数；文件缓存页没有独立计数 */

    /* 累计计数器。注意：这几个是 64 位 sysctl（u_long），必须按 8 字节读，
     * 用 4 字节缓冲会得到 ENOMEM。 */
    guint64 faults;       /* v_vm_faults：缺页总数（软+硬） */
    guint64 io_faults;    /* v_io_faults：需要读盘的缺页（硬缺页） */
    guint64 reactivated;  /* v_reactivated：从 inactive/laundry 重新激活 */
    guint64 pdpages;      /* v_pdpages：page daemon 扫描的页数 */
    guint64 pageins;      /* vnode + swap 换入 */
    guint64 pageouts;     /* vnode + swap 换出 */
} VmStats;

/* 队列在堆叠条里的顺序（颜色也按这个顺序） */
enum {
    VM_SEG_ACTIVE = 0,
    VM_SEG_INACTIVE,
    VM_SEG_LAUNDRY,
    VM_SEG_WIRED,
    VM_SEG_CACHE,     /* 显示为“ZFS 缓存”，取自 arc，需从 wired 中扣除 */
    VM_SEG_FREE,
    VM_SEG_COUNT
};

/* 读取当前 VM 统计；sysctl 不可用时返回 FALSE。 */
gboolean vm_stats_read(VmStats *out);

/* 计算各队列占总页数的比例（纯函数，便于单测）。
 * 总和为 0 时全部返回 0；各值按顺序累加并钳制不超过 1.0。 */
/* 按队列拆出各段字节数：Wired 会先扣掉 ARC，ARC 单独放进“缓存”段。
 * 条形图和图例都从这里取，避免两处各算一遍。 */
void vm_stats_segment_bytes(const VmStats *s, guint64 bytes[VM_SEG_COUNT]);

void vm_stats_fractions(const VmStats *s, double frac[VM_SEG_COUNT]);

const char *vm_stats_seg_name(int seg);

#endif /* VM_STATS_H */
