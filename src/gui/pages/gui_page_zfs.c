/*
 * gui_page_zfs.c - ZFS 池页右上摘要与详情卡片
 */

#include "gui_page_zfs.h"
#include "gui_zfs_arc.h"
#include "gui_zfs_zpool.h"
#include "ui_metrics.h"

#include "gui_state.h"
#include "gui_util.h"
#include "zpool_health.h"
#include "zfs_stats.h"
#include "disk_queue.h"
#include "gui_widgets.h"

#include "combo_chart.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

static GtkWidget *label_x(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_widget_set_halign(l, (xalign >= 1.0f) ? GTK_ALIGN_END : GTK_ALIGN_START);
    return l;
}

static guint64    s_roll_dh[60], s_roll_dm[60];
static int        s_roll_pos, s_roll_cnt;
static double     s_roll_hist[FMON_HISTORY_POINTS];
static uint64_t   s_roll_time[FMON_HISTORY_POINTS];
static CurveData  s_roll_cd = {
    .history = s_roll_hist,
    .time_us = s_roll_time,
    .unit    = "%",
    .max_val = 100.0,
    .r = 0.00, .g = 1.00, .b = 0.00,
    .empty_hint = "开始采集后显示近 60 秒命中率",
    .min_val    = -1.0,
};
static GtkWidget *s_roll_area = NULL;

GtkWidget *gui_zhit_roll_chart(void)
{
    s_roll_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    s_roll_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_roll_area, -1, 180);
    gtk_widget_set_hexpand(s_roll_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_roll_area),
                                   gui_chart_draw, &s_roll_cd, NULL);
    gui_chart_attach_interactions(s_roll_area, &s_roll_cd);
    return s_roll_area;
}

static GtkWidget *arc_row(const char *key, GtkWidget *val)
{
    GtkWidget *h = gui_fixed_row(key, val, 78, 26);

    gtk_widget_set_margin_top(h, FMON_SPACE_1);
    return h;
}

static GtkWidget *s_zhit_val[11];

GtkWidget *gui_zhit_detail_box(void)
{
    static const char *const keys[11] = {
        "总命中率", "数据命中率", "元数据命中率", "命中/秒",
        "预取命中率", "MRU 幽灵/秒", "MFU 幽灵/秒", "未命中/秒",
        "L2 命中率", "淘汰跳过", "哈希冲突",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);
    gtk_box_append(GTK_BOX(box), label_x(_("命中率明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_8);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, FMON_SPACE_1);
    gtk_widget_set_size_request(c1, 300, -1);
    gtk_widget_set_size_request(c2, 300, -1);
    gtk_widget_set_hexpand(c3, TRUE);

    for (int i = 0; i < 11; i++) {
        s_zhit_val[i] = label_x("—", "detail-value", 0.0f);

        GtkWidget *col = (i < 4) ? c1 : (i < 8) ? c2 : c3;

        gtk_box_append(GTK_BOX(col), arc_row(keys[i], s_zhit_val[i]));
    }

    gtk_box_append(GTK_BOX(cols), c1);
    gtk_box_append(GTK_BOX(cols), c2);
    gtk_box_append(GTK_BOX(cols), c3);
    gtk_box_append(GTK_BOX(box), cols);
    return box;
}

void gui_zhit_detail_update(void)
{
    static guint64 prev_mru, prev_mfu, prev_hits, prev_misses;
    static gint64  prev_us;

    if (s_zhit_val[0] == NULL || !gtk_widget_get_mapped(s_zhit_val[0]))
        return;

    const ZfsStats *zs = zfs_stats_get();

    if (zs == NULL) {
        for (int i = 0; i < 11; i++)
            gtk_label_set_text(GTK_LABEL(s_zhit_val[i]), "—");
        return;
    }

    char b[96];

#define SETZ(i, ...)                                                    \
    do {                                                                \
        g_snprintf(b, sizeof(b), __VA_ARGS__);                          \
        gtk_label_set_text(GTK_LABEL(s_zhit_val[i]), b);                \
    } while (0)

    SETZ(0, "%.2f %%", zfs_stats_hit_pct(zs->hits, zs->misses));
    SETZ(1, "%.2f %%", zfs_stats_hit_pct(zs->demand_data_hits,
                                         zs->demand_data_misses));
    SETZ(2, "%.2f %%", zfs_stats_hit_pct(zs->demand_meta_hits,
                                         zs->demand_meta_misses));
    SETZ(4, "%.2f %%", zfs_stats_hit_pct(zs->prefetch_data_hits +
                                            zs->prefetch_meta_hits,
                                        zs->prefetch_data_misses +
                                            zs->prefetch_meta_misses));

    gint64 now = g_get_monotonic_time();
    double dt  = (prev_us > 0) ? (double)(now - prev_us) / 1e6 : 0.0;

    double hits_s   = (dt > 0.0) ? zfs_stats_rate(zs->hits, prev_hits, dt)
                                 : 0.0;
    double misses_s = (dt > 0.0) ? zfs_stats_rate(zs->misses, prev_misses, dt)
                                 : 0.0;

    guint64 dh = (zs->hits >= prev_hits) ? zs->hits - prev_hits : 0;
    guint64 dm = (zs->misses >= prev_misses) ? zs->misses - prev_misses : 0;

    SETZ(3, "%.0f 次/秒", hits_s);
    SETZ(7, "%.0f 次/秒", misses_s);

    SETZ(5, "%.1f 次/秒",
         (dt > 0.0) ? zfs_stats_rate(zs->mru_ghost_hits, prev_mru, dt) : 0.0);
    SETZ(6, "%.1f 次/秒",
         (dt > 0.0) ? zfs_stats_rate(zs->mfu_ghost_hits, prev_mfu, dt) : 0.0);

    prev_mru    = zs->mru_ghost_hits;
    prev_mfu    = zs->mfu_ghost_hits;
    prev_hits   = zs->hits;
    prev_misses = zs->misses;
    prev_us     = now;

    /* 60 秒滚动命中率 */
    s_roll_dh[s_roll_pos] = dh;
    s_roll_dm[s_roll_pos] = dm;
    s_roll_pos = (s_roll_pos + 1) % 60;

    if (s_roll_cnt < 60)
        s_roll_cnt++;

    guint64 sh = 0, sm = 0;

    for (int i = 0; i < s_roll_cnt; i++) {
        sh += s_roll_dh[i];
        sm += s_roll_dm[i];
    }

    double roll = (sh + sm > 0) ? 100.0 * (double)sh /
                                      (double)(sh + sm)
                                : 0.0;

    s_roll_hist[s_roll_cd.index] = roll;
    s_roll_time[s_roll_cd.index] = now;
    s_roll_cd.index = (s_roll_cd.index + 1) % FMON_HISTORY_POINTS;

    if (s_roll_cd.count < FMON_HISTORY_POINTS)
        s_roll_cd.count++;

    s_roll_cd.end_time_us = now;

    if (s_roll_area != NULL)
        gtk_widget_queue_draw(s_roll_area);

    if (zs->l2_hits + zs->l2_misses > 0)
        SETZ(8, "%.2f %%", zfs_stats_hit_pct(zs->l2_hits, zs->l2_misses));
    else
        SETZ(8, "%s", "无 L2");

    SETZ(9, "%llu 次", (unsigned long long)zs->evict_skip);
    SETZ(10, "%llu 次", (unsigned long long)zs->hash_collisions);

#undef SETZ
}

/* ===========================================================================
 * ZFS 带宽明细：三列 + 延迟直方图
 * ========================================================================= */
static GtkWidget *s_zbw_val[8];
static GtkWidget *s_zbw_lat;
static GtkWidget *s_zbw_req;
static GtkWidget *s_zbw_vdevs;

GtkWidget *gui_zbw_detail_box(void)
{
    static const char *const keys[8] = {
        "读取累计", "读取操作", "写入累计", "写入操作",
        "设备队列", "设备繁忙", "逻辑写入/秒", "写放大",
    };

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);
    gtk_box_append(GTK_BOX(box), label_x(_("带宽明细"), "detail-title", 0.0f));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FMON_SPACE_8);
    GtkWidget *c1   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c2   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *c3   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_margin_top(cols, FMON_SPACE_1);
    gtk_widget_set_size_request(c1, 300, -1);
    gtk_widget_set_size_request(c2, 300, -1);
    gtk_widget_set_hexpand(c3, TRUE);

    for (int i = 0; i < 8; i++) {
        s_zbw_val[i] = label_x("—", "detail-value", 0.0f);

        GtkWidget *col = (i < 3) ? c1 : (i < 6) ? c2 : c3;

        gtk_box_append(GTK_BOX(col), arc_row(keys[i], s_zbw_val[i]));
    }

    gtk_box_append(GTK_BOX(cols), c1);
    gtk_box_append(GTK_BOX(cols), c2);
    gtk_box_append(GTK_BOX(cols), c3);
    gtk_box_append(GTK_BOX(box), cols);

    s_zbw_lat = label_x("—", "group-desc", 0.0f);
    gtk_label_set_wrap(GTK_LABEL(s_zbw_lat), TRUE);
    gtk_widget_set_margin_top(s_zbw_lat, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_zbw_lat);

    s_zbw_req = label_x("—", "group-desc", 0.0f);
    gtk_label_set_wrap(GTK_LABEL(s_zbw_req), TRUE);
    gtk_widget_set_margin_top(s_zbw_req, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_zbw_req);

    s_zbw_vdevs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(s_zbw_vdevs, FMON_SPACE_1);
    gtk_box_append(GTK_BOX(box), s_zbw_vdevs);
    return box;
}

void gui_zbw_detail_update(void)
{
    if (s_zbw_val[0] == NULL || !gtk_widget_get_mapped(s_zbw_val[0]))
        return;

    /* 带宽页自己触发 zpool 采样，否则只有访问“池空间”页才会更新 */
    zpool_health_update();

    const FmonShmData *snap = gui_state_snapshot();
    const FmonPoolInfo *z = (snap->pool_count > 0) ? &snap->pools[0] : NULL;
    const ZpoolHealth *h = zpool_health_get();

    guint64 rb = z ? z->read_bytes : 0;
    guint64 wb = z ? z->write_bytes : 0;

    char *rs = fmon_gui_format_bytes(rb);
    char *ws = fmon_gui_format_bytes(wb);

    gtk_label_set_text(GTK_LABEL(s_zbw_val[0]), rs);
    gtk_label_set_text(GTK_LABEL(s_zbw_val[2]), ws);
    g_free(rs);
    g_free(ws);

    char b[96];

    g_snprintf(b, sizeof(b), "%llu", (unsigned long long)(z ? z->read_ops : 0));
    gtk_label_set_text(GTK_LABEL(s_zbw_val[1]), b);

    g_snprintf(b, sizeof(b), "%llu",
               (unsigned long long)(z ? z->write_ops : 0));
    gtk_label_set_text(GTK_LABEL(s_zbw_val[3]), b);

    /* 设备队列长度（iostat -x 的 qlen）与繁忙度，替代内核不导出的 pend/activ */
    const DiskQueue *dq = disk_queue_get();

    if (dq != NULL) {
        GString *sq = g_string_new(NULL);

        for (int i = 0; i < dq->n && i < 2; i++)
            g_string_append_printf(sq, "%s%s %.0f", (i > 0) ? " · " : "",
                                   dq->dev[i].name, dq->dev[i].qlen);

        gtk_label_set_text(GTK_LABEL(s_zbw_val[4]), sq->str);
        g_string_free(sq, TRUE);

        g_snprintf(b, sizeof(b), "%.0f %%", dq->dev[0].busy_pct);
        gtk_label_set_text(GTK_LABEL(s_zbw_val[5]), b);
    } else {
        gtk_label_set_text(GTK_LABEL(s_zbw_val[4]), "—");
        gtk_label_set_text(GTK_LABEL(s_zbw_val[5]), "—");
    }

    /* 逻辑写速率（数据集 kstat，无 zfs list 开销）与写放大 */
    static guint64 prev_written, prev_phys;
    static gint64  prev_us;

    if (h != NULL && h->ds_written > 0) {
        gint64 now = g_get_monotonic_time();

        if (prev_us == 0) {
            prev_written = h->ds_written;
            prev_us      = now;
        } else if (h->ds_written > prev_written) {
            /* 只在 kstat 更新（15 秒一次）时计算，避免每秒把差值摊薄为 0 */
            double dt = (double)(now - prev_us) / 1e6;
            double lw = (dt > 0.0)
                            ? (double)(h->ds_written - prev_written) / dt
                            : 0.0;

            prev_written = h->ds_written;
            prev_us      = now;

            char *ls = fmon_gui_format_rate(lw / 1024.0);

            gtk_label_set_text(GTK_LABEL(s_zbw_val[6]), ls);
            g_free(ls);

            /* 物理写速率用池累计写字节的同一时间窗口差分，避免
             * “1 秒瞬时 / 15 秒平均”混算导致放大比失真 */
            guint64 pw = (snap->pool_count > 0) ? snap->pools[0].write_bytes : 0;
            double phys = (dt > 0.0 && pw >= prev_phys)
                              ? (double)(pw - prev_phys) / dt
                              : 0.0;

            prev_phys = pw;

            if (lw > 100.0 * 1024.0 && phys > 0.0)
                g_snprintf(b, sizeof(b), "%.2fx", phys / lw);
            else
                g_snprintf(b, sizeof(b), _("—（当前写入过低）"));

            gtk_label_set_text(GTK_LABEL(s_zbw_val[7]), b);
        }
    }

    /* 延迟直方图：只显示计数 > 0 的桶，避免一行太长 */
    GString *g = g_string_new(_("延迟直方图  "));

    if (h != NULL) {
        for (int i = 0; i < h->lat_n; i++) {
            if (h->lat[i].count <= 0.0)
                continue;

            g_string_append_printf(g, "%s %.0f   ", h->lat[i].label,
                                   h->lat[i].count);
        }
    }

    if (g->len <= strlen(_("延迟直方图  ")))
        g_string_append(g, _("无数据（空闲）"));

    gtk_label_set_text(GTK_LABEL(s_zbw_lat), g->str);
    g_string_free(g, TRUE);

    /* 每个 vdev 的延迟（替代不可用的 vdev 队列）；内容不变就不重建 */
    static gchar *s_zbw_vdev_sig;

    if (s_zbw_vdevs != NULL) {
        GString *sig = g_string_new(NULL);

        if (h != NULL) {
            for (int i = 0; i < h->vdev_count; i++)
                g_string_append_printf(sig, "%s|%.2f|%.2f|%.2f|%.2f;",
                                       h->vdev[i].name, h->vdev[i].tw_r_ms,
                                       h->vdev[i].tw_w_ms, h->vdev[i].dw_r_ms,
                                       h->vdev[i].dw_w_ms);
        }

        if (g_strcmp0(sig->str, s_zbw_vdev_sig) == 0) {
            g_string_free(sig, TRUE);
            goto vdev_done;
        }

        g_free(s_zbw_vdev_sig);
        s_zbw_vdev_sig = g_strdup(sig->str);
        g_string_free(sig, TRUE);

        GtkWidget *child = NULL;

        while ((child = gtk_widget_get_first_child(s_zbw_vdevs)) != NULL)
            gtk_box_remove(GTK_BOX(s_zbw_vdevs), child);

        if (h != NULL && h->vdev_count > 0) {
            for (int i = 0; i < h->vdev_count; i++) {
                const char *nm = h->vdev[i].name;

                gchar *txt = g_strdup_printf(
                    _("%s  延迟 读 %.2f / 写 %.2f ms · disk 读 %.2f / 写 %.2f ms"),
                    nm, h->vdev[i].tw_r_ms, h->vdev[i].tw_w_ms,
                    h->vdev[i].dw_r_ms, h->vdev[i].dw_w_ms);

                gtk_box_append(GTK_BOX(s_zbw_vdevs),
                               label_x(txt, "group-desc", 0.0f));
                g_free(txt);
            }
        } else {
            gtk_box_append(GTK_BOX(s_zbw_vdevs),
                           label_x(_("vdev 延迟：采集中…"), "group-desc",
                                   0.0f));
        }
    }

vdev_done:
    /* 写请求大小分布（iostat -r 聚合） */
    if (h != NULL && s_zbw_req != NULL) {
        gchar *rq = g_strdup_printf(
            _("写请求大小  ≤4K %.0f · 8–64K %.0f · ≥128K %.0f"),
            h->req_small, h->req_mid, h->req_large);

        gtk_label_set_text(GTK_LABEL(s_zbw_req), rq);
        g_free(rq);
    }
}
