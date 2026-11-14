/*
 * mem_summary.c - 内存页说明行
 */

#include "mem_summary.h"

#include <glib/gi18n.h>

static double to_gb(guint64 bytes)
{
    return (double)bytes / (1024.0 * 1024.0 * 1024.0);
}

double gui_mem_effective_used_pct(guint64 total_bytes, guint64 avail_bytes,
                                  guint64 arc_bytes)
{
    if (total_bytes == 0)
        return 0.0;

    if (arc_bytes > total_bytes)      /* 计数异常时钳制，避免无符号下溢 */
        arc_bytes = total_bytes;

    guint64 reclaimable = avail_bytes;

    if (reclaimable > total_bytes - arc_bytes)
        reclaimable = total_bytes - arc_bytes;

    guint64 used = total_bytes - reclaimable - arc_bytes;

    return 100.0 * (double)used / (double)total_bytes;
}

gchar *gui_mem_buf_text(guint64 bufspace_bytes)
{
    if (bufspace_bytes == 0)
        return g_strdup("");

    double gb = to_gb(bufspace_bytes);

    if (gb >= 1.0)
        return g_strdup_printf(_("缓冲（Buf）%.2f GB"), gb);

    return g_strdup_printf(_("缓冲（Buf）%.1f MB"),
                           (double)bufspace_bytes / (1024.0 * 1024.0));
}

gchar *gui_mem_summary_text(guint64 total_bytes, guint64 avail_bytes,
                            guint64 arc_bytes)
{
    if (total_bytes == 0 || arc_bytes < MEM_SUMMARY_ARC_MIN_BYTES)
        return g_strdup("");

    double total = to_gb(total_bytes);
    double avail = to_gb(avail_bytes);
    double arc   = to_gb(arc_bytes);
    double used  = total - avail;
    double real  = avail + arc;

    if (used < 0.0)
        used = 0.0;
    if (real > total)
        real = total;

    return g_strdup_printf(
        _("已用 %.2f GB（其中 ZFS ARC %.2f GB 可回收）｜ 实际可用约 %.2f GB"),
        used, arc, real);
}
