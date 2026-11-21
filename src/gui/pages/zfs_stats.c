/*
 * zfs_stats.c - ZFS ARC 细节采样
 */

#include "zfs_stats.h"

#include <string.h>
#include <sys/sysctl.h>

static ZfsStats g_stats;
static gboolean g_valid = FALSE;

static guint64   g_prev_prune, g_prev_l2r, g_prev_l2w;
static gint64    g_prev_us = 0;

static guint64 rd64(const char *name)
{
    guint64 v   = 0;
    size_t  len = sizeof(v);

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return 0;

    return v;
}

ZfsReclaimState zfs_stats_reclaim_state(double prune_s, double skip_s,
                                        guint64 throttle_delta,
                                        guint64 no_grow)
{
    gboolean burst    = (prune_s >= 1.0 || skip_s >= 1.0);
    gboolean pressure = (throttle_delta > 0) || (no_grow > 0);

    if (!burst)
        return ZFS_RECLAIM_IDLE;

    return pressure ? ZFS_RECLAIM_PRESSURE : ZFS_RECLAIM_NORMAL;
}

double zfs_stats_hit_pct(guint64 hits, guint64 misses)
{
    guint64 total = hits + misses;

    if (total == 0)
        return 0.0;

    return 100.0 * (double)hits / (double)total;
}

double zfs_stats_rate(guint64 cur, guint64 prev, double dt)
{
    if (dt <= 0.0 || cur < prev)
        return 0.0;

    return (double)(cur - prev) / dt;
}

void zfs_stats_collect(ZfsStats *out)
{
    if (out == NULL)
        return;

    ZfsStats s;

    memset(&s, 0, sizeof(s));

#define ZFS_READ(field, suffix) \
    s.field = rd64("kstat.zfs.misc.arcstats." suffix)

    ZFS_READ(size, "size");
    ZFS_READ(c_max, "c_max");
    ZFS_READ(mru_size, "mru_size");
    ZFS_READ(mfu_size, "mfu_size");
    ZFS_READ(meta_used, "arc_meta_used");
    ZFS_READ(l2_size, "l2_size");

    ZFS_READ(dnode_size, "dnode_size");
    ZFS_READ(dbuf_size, "dbuf_size");
    ZFS_READ(metadata_size, "metadata_size");
    ZFS_READ(data_size, "data_size");
    ZFS_READ(hdr_size, "hdr_size");
    ZFS_READ(c, "c");
    ZFS_READ(c_min, "c_min");

    ZFS_READ(hits, "hits");
    ZFS_READ(misses, "misses");
    ZFS_READ(demand_data_hits, "demand_data_hits");
    ZFS_READ(demand_meta_hits, "demand_metadata_hits");
    ZFS_READ(prefetch_data_hits, "prefetch_data_hits");
    ZFS_READ(prefetch_meta_hits, "prefetch_metadata_hits");
    ZFS_READ(demand_data_misses, "demand_data_misses");
    ZFS_READ(demand_meta_misses, "demand_metadata_misses");
    ZFS_READ(prefetch_data_misses, "prefetch_data_misses");
    ZFS_READ(prefetch_meta_misses, "prefetch_metadata_misses");
    ZFS_READ(mru_ghost_hits, "mru_ghost_hits");
    ZFS_READ(mfu_ghost_hits, "mfu_ghost_hits");
    ZFS_READ(l2_hits, "l2_hits");
    ZFS_READ(l2_misses, "l2_misses");

    ZFS_READ(arc_prune, "arc_prune");
    ZFS_READ(evict_skip, "evict_skip");
    ZFS_READ(evict_not_enough, "evict_not_enough");
    ZFS_READ(mem_throttle, "memory_throttle_count");
    ZFS_READ(hash_collisions, "hash_collisions");

    ZFS_READ(access_skip, "access_skip");
    ZFS_READ(mutex_miss, "mutex_miss");
    ZFS_READ(deleted, "deleted");

    ZFS_READ(no_grow, "arc_no_grow");
    ZFS_READ(need_free, "arc_need_free");
    ZFS_READ(tempreserve, "arc_tempreserve");
    ZFS_READ(sys_free, "arc_sys_free");
    ZFS_READ(mem_free, "memory_free_bytes");
    ZFS_READ(mem_avail, "memory_available_bytes");
    ZFS_READ(mem_all, "memory_all_bytes");
    ZFS_READ(mem_direct, "memory_direct_count");
    ZFS_READ(mem_indirect, "memory_indirect_count");

    ZFS_READ(mru_evictable_data, "mru_evictable_data");
    ZFS_READ(mru_evictable_meta, "mru_evictable_metadata");
    ZFS_READ(mfu_evictable_data, "mfu_evictable_data");
    ZFS_READ(mfu_evictable_meta, "mfu_evictable_metadata");
    ZFS_READ(anon_evictable_data, "anon_evictable_data");
    ZFS_READ(anon_evictable_meta, "anon_evictable_metadata");
    ZFS_READ(anon_size, "anon_size");
    ZFS_READ(mru_ghost_size, "mru_ghost_size");
    ZFS_READ(mfu_ghost_size, "mfu_ghost_size");

    ZFS_READ(hash_elements, "hash_elements");
    ZFS_READ(hash_elements_max, "hash_elements_max");
    ZFS_READ(hash_chains, "hash_chains");
    ZFS_READ(l2_read_bytes, "l2_read_bytes");
    ZFS_READ(l2_write_bytes, "l2_write_bytes");

    ZFS_READ(l2_ndev, "l2_ndev");
    ZFS_READ(l2_asize, "l2_asize");
    ZFS_READ(l2_hdr_size, "l2_hdr_size");
    ZFS_READ(l2_writes_sent, "l2_writes_sent");
    ZFS_READ(l2_writes_done, "l2_writes_done");
    ZFS_READ(l2_writes_error, "l2_writes_error");
    ZFS_READ(l2_evict_reading, "l2_evict_reading");
    ZFS_READ(l2_evict_l1cached, "l2_evict_l1cached");
    ZFS_READ(l2_evict_lock_retry, "l2_evict_lock_retry");
    ZFS_READ(l2_free_on_write, "l2_free_on_write");
    ZFS_READ(l2_io_error, "l2_io_error");
    ZFS_READ(l2_cksum_bad, "l2_cksum_bad");
    ZFS_READ(l2_abort_lowmem, "l2_abort_lowmem");
    ZFS_READ(l2_feeds, "l2_feeds");
    ZFS_READ(l2_rw_clash, "l2_rw_clash");
    ZFS_READ(l2_rebuild_success, "l2_rebuild_success");
    ZFS_READ(l2_rebuild_size, "l2_rebuild_size");
    ZFS_READ(l2_rebuild_io_errors, "l2_rebuild_io_errors");
    ZFS_READ(l2_mfu_asize, "l2_mfu_asize");
    ZFS_READ(l2_mru_asize, "l2_mru_asize");
    ZFS_READ(l2_prefetch_asize, "l2_prefetch_asize");

    ZFS_READ(compressed_size, "compressed_size");
    ZFS_READ(uncompressed_size, "uncompressed_size");

#undef ZFS_READ

    gint64 now = g_get_monotonic_time();
    double dt  = (g_prev_us > 0) ? (double)(now - g_prev_us) / 1e6 : 0.0;

    s.prune_per_s  = zfs_stats_rate(s.arc_prune, g_prev_prune, dt);
    s.l2_read_bps  = zfs_stats_rate(s.l2_read_bytes, g_prev_l2r, dt);
    s.l2_write_bps = zfs_stats_rate(s.l2_write_bytes, g_prev_l2w, dt);

    g_prev_prune = s.arc_prune;
    g_prev_l2r   = s.l2_read_bytes;
    g_prev_l2w   = s.l2_write_bytes;
    g_prev_us    = now;

    *out = s;
}

void zfs_stats_publish(const ZfsStats *in)
{
    if (in == NULL)
        return;

    /* ZFS 未加载时所有字段都是 0：视为不可用，页面显示“—” */
    g_valid = (in->size != 0 || in->c_max != 0);

    if (g_valid)
        g_stats = *in;
}

void zfs_stats_update(void)
{
    ZfsStats s;

    memset(&s, 0, sizeof(s));
    zfs_stats_collect(&s);
    zfs_stats_publish(&s);
}

const ZfsStats *zfs_stats_get(void)
{
    return g_valid ? &g_stats : NULL;
}
