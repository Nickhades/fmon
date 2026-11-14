/*
 * vm_stats.c - FreeBSD 虚拟内存队列明细
 */

#include "vm_stats.h"

#include <string.h>

#include <sys/types.h>
#include <sys/sysctl.h>

/* 统一按 8 字节读：4 字节的计数器内核会返回 len=4（高 32 位保持 0），
 * 64 位的计数器（如 v_pdpages）用 4 字节缓冲会 ENOMEM。 */
static gboolean rd64(const char *name, guint64 *out)
{
    guint64 v   = 0;
    size_t  len = sizeof(v);

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return FALSE;

    *out = v;
    return TRUE;
}

gboolean vm_stats_read(VmStats *out)
{
    if (out == NULL)
        return FALSE;

    VmStats s;
    guint64 page_size = 0, page_count = 0;

    memset(&s, 0, sizeof(s));

    if (!rd64("hw.pagesize", &page_size) || page_size == 0)
        return FALSE;
    if (!rd64("vm.stats.vm.v_page_count", &page_count) || page_count == 0)
        return FALSE;

    guint64 active = 0, inactive = 0, laundry = 0, wired = 0, bufspace = 0;
    guint64 free_pages = 0, reactivated = 0, pdpages = 0, pins = 0, pouts = 0;
    guint64 faults = 0, io_faults = 0;
    guint64 vnode_in = 0, vnode_out = 0, swap_in = 0, swap_out = 0;

    (void)rd64("vm.stats.vm.v_active_count", &active);
    (void)rd64("vm.stats.vm.v_inactive_count", &inactive);
    (void)rd64("vm.stats.vm.v_laundry_count", &laundry);
    (void)rd64("vm.stats.vm.v_wire_count", &wired);
    (void)rd64("vm.stats.vm.v_free_count", &free_pages);
    (void)rd64("vfs.bufspace", &bufspace);
    (void)rd64("vm.stats.vm.v_vm_faults", &faults);
    (void)rd64("vm.stats.vm.v_io_faults", &io_faults);
    (void)rd64("vm.stats.vm.v_reactivated", &reactivated);
    (void)rd64("vm.stats.vm.v_pdpages", &pdpages);
    (void)rd64("vm.stats.vm.v_vnodepgsin", &vnode_in);
    (void)rd64("vm.stats.vm.v_vnodepgsout", &vnode_out);
    (void)rd64("vm.stats.vm.v_swappgsin", &swap_in);
    (void)rd64("vm.stats.vm.v_swappgsout", &swap_out);

    pins  = vnode_in + swap_in;
    pouts = vnode_out + swap_out;

    s.page_size   = page_size;
    s.total       = page_count * page_size;
    s.active      = active * page_size;
    s.inactive    = inactive * page_size;
    s.laundry     = laundry * page_size;
    s.wired       = wired * page_size;
    s.free        = free_pages * page_size;
    s.bufspace    = bufspace;
    s.faults      = faults;
    s.io_faults   = io_faults;
    s.reactivated = reactivated;
    s.pdpages     = pdpages;
    s.pageins     = pins;
    s.pageouts    = pouts;

    *out = s;
    return TRUE;
}

void vm_stats_segment_bytes(const VmStats *s, guint64 bytes[VM_SEG_COUNT])
{
    memset(bytes, 0, sizeof(guint64) * VM_SEG_COUNT);

    if (s == NULL)
        return;

    /* ARC 属于 Wired：从 Wired 里扣掉，单独作为“ZFS 缓存”段，
     * 否则同一块内存在条形里会出现两次。 */
    guint64 arc  = (s->arc < s->wired) ? s->arc : s->wired;
    guint64 wire = s->wired - arc;

    bytes[VM_SEG_ACTIVE]   = s->active;
    bytes[VM_SEG_INACTIVE] = s->inactive;
    bytes[VM_SEG_LAUNDRY]  = s->laundry;
    bytes[VM_SEG_WIRED]    = wire;
    bytes[VM_SEG_CACHE]    = arc;
    bytes[VM_SEG_FREE]     = s->free;
}

void vm_stats_fractions(const VmStats *s, double frac[VM_SEG_COUNT])
{
    memset(frac, 0, sizeof(double) * VM_SEG_COUNT);

    if (s == NULL || s->total == 0)
        return;

    guint64 v[VM_SEG_COUNT];

    vm_stats_segment_bytes(s, v);

    double sum = 0.0;

    /* 单个队列的占比；极端情况下（各计数之和超过 page_count）
     * 保证总和不超过 1.0，避免堆叠条画出边界。 */
    for (int i = 0; i < VM_SEG_COUNT; i++) {
        double f = (double)v[i] / (double)s->total;

        if (sum + f > 1.0)
            f = 1.0 - sum;
        if (f < 0.0)
            f = 0.0;

        frac[i] = f;
        sum += f;
    }
}

const char *vm_stats_seg_name(int seg)
{
    static const char *const names[VM_SEG_COUNT] = {
        "活跃", "非活跃", "待回收", "常驻", "ZFS 缓存", "空闲",
    };

    if (seg < 0 || seg >= VM_SEG_COUNT)
        return "?";

    return names[seg];
}
