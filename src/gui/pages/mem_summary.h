/*
 * mem_summary.h - 内存页说明行（纯函数，便于单测）
 *
 * 把“已用 / ZFS ARC 可回收 / 实际可用”这段文案的生成从 GTK 里拆出来，
 * 这样可以在 tests/ 里直接测（不依赖 GTK、不依赖运行中的 daemon）。
 */

#ifndef MEM_SUMMARY_H
#define MEM_SUMMARY_H

#include <glib.h>

/* ARC 小于这个值时认为“没有可回收的 ZFS 缓存”，说明行留空，
 * 避免无 ZFS 的机器上出现 "ZFS ARC 0.00 GB" 的噪音。 */
#define MEM_SUMMARY_ARC_MIN_BYTES (50ULL * 1024 * 1024)

/* 返回新分配的说明文本；不需要显示时返回空字符串（非 NULL）。
 * 调用者负责 g_free()。 */
gchar *gui_mem_summary_text(guint64 total_bytes, guint64 avail_bytes,
                            guint64 arc_bytes);

/* 缓冲缓存（vfs.bufspace，top 里的 Buf）。FreeBSD 没有 Linux 那种
 * buff/cache 汇总项，这是唯一显式计数的“缓存”；为 0 时返回空串。 */
gchar *gui_mem_buf_text(guint64 bufspace_bytes);

/* FreeBSD 口径的内存使用率：把“可用 + ZFS ARC”都视为可回收，
 * 避免 ARC 把 90% 阈值顶爆（告警应与 FreeBSD 的 VM 语义对齐）。
 * 纯函数，便于单测；total 为 0 时返回 0。 */
double gui_mem_effective_used_pct(guint64 total_bytes, guint64 avail_bytes,
                                  guint64 arc_bytes);

#endif /* MEM_SUMMARY_H */
