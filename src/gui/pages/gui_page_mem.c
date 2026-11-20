/*
 * gui_page_mem.c - 内存条卡片与刷新
 *
 * 由 main.c 拆出：数据来源于 gui_state_snapshot() 的一致快照。
 */

#include "gui_page_mem.h"

#include "gui_state.h"
#include "gui_util.h"
#include "gui_widgets.h"
#include "mem_summary.h"
#include "vm_stats.h"
#include "zfs_stats.h"
#include "kmem_stats.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>
#include <string.h>

static GtkWidget *s_mod_box = NULL;   /* 标题行右侧的内存条摘要 */
static gchar     *s_mod_sig = NULL;

/* ===========================================================================
 * 内存队列卡片
 * ========================================================================= */
static GtkWidget *s_qbar   = NULL;
static GtkWidget *s_qval[VM_SEG_COUNT];
/* 分组数值：回收压力 / 磁盘 I/O / ZFS 缓存 / 内核内存 */
static GtkWidget *s_rate_val[6];
static GtkWidget *s_zfs_val[5];
static GtkWidget *s_kmem_val[5];
static VmStats    s_qstats;
static gboolean   s_qvalid = FALSE;
static guint64    s_prev_re, s_prev_sr, s_prev_po;
static guint64    s_prev_pi, s_prev_flt, s_prev_iof;
static gint64     s_prev_us = 0;

static const double SEG_COLOR[VM_SEG_COUNT][3] = {
    { 0.26, 0.55, 0.95 },  /* Active   */
    { 0.20, 0.70, 0.84 },  /* Inactive */
    { 0.95, 0.70, 0.25 },  /* Laundry  */
    { 0.62, 0.48, 0.86 },  /* Wired    */
    { 0.35, 0.74, 0.46 },  /* Cache    */
    { 0.72, 0.74, 0.78 },  /* Free     */
};

static const char *const SEG_HEX[VM_SEG_COUNT] = {
    "#4290f2", "#33b3d6", "#f2b340", "#9e7adb", "#59bd75", "#b8bcc7",
};

static void queue_bar_draw(GtkDrawingArea *area, cairo_t *cr, int width,
                           int height, gpointer data)
{
    (void)area;
    (void)data;

    if (!s_qvalid || width <= 0 || height <= 0)
        return;

    double frac[VM_SEG_COUNT];

    vm_stats_fractions(&s_qstats, frac);

    double x = 0.0;

    for (int i = 0; i < VM_SEG_COUNT; i++) {
        double w = frac[i] * width;

        if (w <= 0.0)
            continue;

        cairo_set_source_rgb(cr, SEG_COLOR[i][0], SEG_COLOR[i][1],
                             SEG_COLOR[i][2]);
        cairo_rectangle(cr, x, 0.0, w, height);
        cairo_fill(cr);
        x += w;
    }
}

static void setv(GtkWidget *w, const char *text)
{
    if (w != NULL && g_strcmp0(gtk_label_get_text(GTK_LABEL(w)), text) != 0)
        gtk_label_set_text(GTK_LABEL(w), text);
}

/* 统一小节：标题 + 单列键值行；键左对齐、固定宽度，值紧随其后。
 * 三栏并排时每一栏内部的键、值都在同一条竖线上，扫描起来最整齐。 */
static GtkWidget *mem_section(const char *title, const char *const *keys,
                              int n, GtkWidget **vals)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t   = gtk_label_new(title);

    gtk_widget_add_css_class(t, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_widget_set_margin_bottom(t, 2);
    gtk_box_append(GTK_BOX(box), t);

    for (int i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        GtkWidget *k = gtk_label_new(keys[i]);

        gtk_widget_add_css_class(k, "group-desc");
        gtk_label_set_xalign(GTK_LABEL(k), 0.0f);
        gtk_widget_set_halign(k, GTK_ALIGN_START);
        gtk_widget_set_size_request(k, 82, -1);

        vals[i] = gtk_label_new("—");
        gtk_widget_add_css_class(vals[i], "detail-value");
        gtk_label_set_xalign(GTK_LABEL(vals[i]), 0.0f);
        gtk_widget_set_halign(vals[i], GTK_ALIGN_START);
        /* 数值列定宽，刷新时列位置不抖 */
        gtk_widget_set_size_request(vals[i], 92, -1);

        gtk_box_append(GTK_BOX(row), k);
        gtk_box_append(GTK_BOX(row), vals[i]);
        gtk_box_append(GTK_BOX(box), row);
    }

    return box;
}

GtkWidget *gui_mem_queues_box(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_widget_set_margin_top(card, 10);

    GtkWidget *t = gtk_label_new(_("内存队列"));
    gtk_widget_add_css_class(t, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(card), t);

    s_qbar = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_qbar, -1, 18);
    gtk_widget_set_margin_top(s_qbar, 4);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_qbar), queue_bar_draw,
                                   NULL, NULL);
    gtk_box_append(GTK_BOX(card), s_qbar);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_widget_set_margin_top(grid, 6);

    for (int i = 0; i < VM_SEG_COUNT; i++) {
        int col = (i % 2) * 2;
        int row = i / 2;

        gchar *markup = g_strdup_printf(
            "<span foreground=\"%s\">■</span>  %s", SEG_HEX[i],
            vm_stats_seg_name(i));
        GtkWidget *name = gtk_label_new(NULL);

        gtk_label_set_markup(GTK_LABEL(name), markup);
        g_free(markup);

        s_qval[i] = gtk_label_new("—");
        gtk_widget_add_css_class(s_qval[i], "detail-value");
        gtk_label_set_xalign(GTK_LABEL(s_qval[i]), 0.0f);
        /* 数值列定宽，图例列不会因 GB/% 变化而抖动 */
        gtk_widget_set_size_request(s_qval[i], 132, -1);
        gtk_widget_set_halign(s_qval[i], GTK_ALIGN_START);

        gtk_grid_attach(GTK_GRID(grid), name, col, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), s_qval[i], col + 1, row, 1, 1);
    }

    gtk_box_append(GTK_BOX(card), grid);

    static const char *const rate_keys[] = {
        "重新激活", "扫描", "换出", "换入", "缺页", "需读盘",
    };
    static const char *const zfs_keys[] = {
        "命中率", "MRU", "MFU", "内存节流", "压缩比",
    };
    static const char *const kmem_keys[] = {
        "UMA 合计", "ZFS", "ZIO", "页表", "缓冲",
    };

    /* 三栏并排：回收与压力 / ZFS 缓存 / 内核内存，各栏内键左对齐 */
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);

    gtk_widget_set_margin_top(cols, 10);

    GtkWidget *left   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *middle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *right  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_hexpand(middle, TRUE);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_valign(left, GTK_ALIGN_START);
    gtk_widget_set_valign(middle, GTK_ALIGN_START);
    gtk_widget_set_valign(right, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(left),
                   mem_section(_("回收与压力"), rate_keys, 6, s_rate_val));
    gtk_box_append(GTK_BOX(middle),
                   mem_section(_("ZFS 缓存"), zfs_keys, 5, s_zfs_val));
    gtk_box_append(GTK_BOX(right),
                   mem_section(_("内核内存（UMA）"), kmem_keys, 5,
                               s_kmem_val));

    gtk_box_append(GTK_BOX(cols), left);
    gtk_box_append(GTK_BOX(cols), middle);
    gtk_box_append(GTK_BOX(cols), right);
    gtk_box_append(GTK_BOX(card), cols);

    return card;
}

void gui_mem_queues_update(void)
{
    if (s_qbar == NULL || !gtk_widget_get_mapped(s_qbar))
        return;   /* 内存队列只在内存页可见时更新 */

    VmStats st;

    if (!vm_stats_read(&st))
        return;

    /* ARC 大小来自快照（与内存页上方说明行同源） */
    st.arc = gui_state_snapshot()->zfs_arc_size;

    s_qstats = st;
    s_qvalid = TRUE;

    gint64 now = g_get_monotonic_time();
    double dt  = (s_prev_us > 0) ? (double)(now - s_prev_us) / 1e6 : 0.0;
    double re = 0.0, sr = 0.0, po = 0.0, pi = 0.0, flt = 0.0, iof = 0.0;

    if (dt > 0.05) {
        /* 64 位累计计数器，直接做无符号差 */
        re  = (double)(st.reactivated - s_prev_re) / dt;
        sr  = (double)(st.pdpages - s_prev_sr) / dt;
        po  = (double)(st.pageouts - s_prev_po) / dt;
        pi  = (double)(st.pageins - s_prev_pi) / dt;
        flt = (double)(st.faults - s_prev_flt) / dt;
        iof = (double)(st.io_faults - s_prev_iof) / dt;
    }

    s_prev_re  = st.reactivated;
    s_prev_sr  = st.pdpages;
    s_prev_po  = st.pageouts;
    s_prev_pi  = st.pageins;
    s_prev_flt = st.faults;
    s_prev_iof = st.io_faults;
    s_prev_us  = now;

    guint64 bytes[VM_SEG_COUNT];
    double  frac[VM_SEG_COUNT];

    vm_stats_segment_bytes(&st, bytes);

    vm_stats_fractions(&st, frac);

    for (int i = 0; i < VM_SEG_COUNT; i++) {
        gchar *txt = g_strdup_printf(
            "%.2f GB · %.1f%%",
            (double)bytes[i] / (1024.0 * 1024.0 * 1024.0), frac[i] * 100.0);

        gtk_label_set_text(GTK_LABEL(s_qval[i]), txt);
        g_free(txt);
    }

    /* 回收与压力：每个指标独立成格，数值对齐 */
    char b[64];

    g_snprintf(b, sizeof(b), "%.0f 页/s", re);
    setv(s_rate_val[0], b);
    g_snprintf(b, sizeof(b), "%.0f 页/s", sr);
    setv(s_rate_val[1], b);
    g_snprintf(b, sizeof(b), "%.0f 页/s", po);
    setv(s_rate_val[2], b);
    g_snprintf(b, sizeof(b), "%.0f 页/s", pi);
    setv(s_rate_val[3], b);
    g_snprintf(b, sizeof(b), "%.0f 次/s", flt);
    setv(s_rate_val[4], b);
    g_snprintf(b, sizeof(b), "%.0f 次/s", iof);
    setv(s_rate_val[5], b);

    /* ZFS 缓存质量 */
    {
        const ZfsStats *zs = zfs_stats_get();

        if (zs != NULL && zs->size > 0) {
            double hit = zfs_stats_hit_pct(zs->hits, zs->misses);
            double both = (double)zs->mru_size + (double)zs->mfu_size;
            double mru = (both > 0.0)
                             ? 100.0 * (double)zs->mru_size / both
                             : 0.0;
            double ratio = (zs->compressed_size > 0)
                               ? (double)zs->uncompressed_size /
                                     (double)zs->compressed_size
                               : 0.0;

            g_snprintf(b, sizeof(b), "%.2f %%", hit);
            setv(s_zfs_val[0], b);
            g_snprintf(b, sizeof(b), "%.0f %%", mru);
            setv(s_zfs_val[1], b);
            g_snprintf(b, sizeof(b), "%.0f %%", 100.0 - mru);
            setv(s_zfs_val[2], b);
            g_snprintf(b, sizeof(b), "%llu 次",
                       (unsigned long long)zs->mem_throttle);
            setv(s_zfs_val[3], b);
            g_snprintf(b, sizeof(b), "%.2f:1", ratio);
            setv(s_zfs_val[4], b);
        } else {
            for (int i = 0; i < 5; i++)
                setv(s_zfs_val[i], "—");
        }
    }

    /* 内核内存（vmstat -z，模块内部 5 秒异步刷新） */
    kmem_stats_update();

    {
        const KmemSummary *km = kmem_stats_get();

        if (km == NULL) {
            for (int i = 0; i < 5; i++)
                setv(s_kmem_val[i], "…");
        } else {
            char *total = fmon_gui_format_bytes(km->total_bytes);
            char *zfs   = fmon_gui_format_bytes(km->zfs_bytes);
            char *zio   = fmon_gui_format_bytes(km->zio_bytes);
            char *page  = fmon_gui_format_bytes(km->page_bytes);
            char *buf   = fmon_gui_format_bytes(st.bufspace);

            setv(s_kmem_val[0], total);
            setv(s_kmem_val[1], zfs);
            setv(s_kmem_val[2], zio);
            setv(s_kmem_val[3], page);
            setv(s_kmem_val[4], buf);

            g_free(total);
            g_free(zfs);
            g_free(zio);
            g_free(page);
            g_free(buf);
        }
    }

    gtk_widget_queue_draw(s_qbar);
}

static GtkWidget *s_totals_mem  = NULL;
static GtkWidget *s_totals_swap = NULL;

GtkWidget *gui_mem_totals_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_halign(box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(box, 6);

    s_totals_mem = gtk_label_new("");
    gtk_widget_add_css_class(s_totals_mem, "hw-model");
    gtk_label_set_xalign(GTK_LABEL(s_totals_mem), 1.0f);
    gtk_widget_set_halign(s_totals_mem, GTK_ALIGN_END);

    s_totals_swap = gtk_label_new("");
    gtk_widget_add_css_class(s_totals_swap, "hw-model");
    gtk_label_set_xalign(GTK_LABEL(s_totals_swap), 1.0f);
    gtk_widget_set_halign(s_totals_swap, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(box), s_totals_mem);
    gtk_box_append(GTK_BOX(box), s_totals_swap);
    return box;
}

void gui_mem_totals_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (s_totals_mem == NULL)
        return;

    char *total = fmon_gui_format_bytes(snap->mem_total_bytes);
    char *used  = fmon_gui_format_bytes(snap->mem_total_bytes -
                                        snap->mem_avail_bytes);
    char *avail = fmon_gui_format_bytes(snap->mem_avail_bytes);

    gchar *m = g_strdup_printf(_("内存 %s · 已用 %s · 可用 %s"),
                               total, used, avail);

    gtk_label_set_text(GTK_LABEL(s_totals_mem), m);
    g_free(m);
    g_free(total);
    g_free(used);
    g_free(avail);

    gchar *s;

    if ((snap->probe_valid & FMON_PROBE_SWAP) == 0) {
        s = g_strdup(_("Swap：不可用"));
    } else {
        double swap_total = (double)snap->swap_total_kb / (1024.0 * 1024.0);
        double swap_used  = (double)snap->swap_used_kb / (1024.0 * 1024.0);

        s = g_strdup_printf(_("Swap 总 %.2f GB · 已用 %.2f GB"), swap_total,
                            swap_used);
    }

    gtk_label_set_text(GTK_LABEL(s_totals_swap), s);
    g_free(s);
}

GtkWidget *gui_mem_modules_header(void)
{
    s_mod_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(s_mod_box, GTK_ALIGN_END);
    gtk_widget_set_valign(s_mod_box, GTK_ALIGN_START);
    return s_mod_box;
}

static GtkWidget *mod_line(const char *text, const char *css, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);

    gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_label_set_wrap(GTK_LABEL(l), wrap);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_RIGHT);
    return l;
}

void gui_mem_modules_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (s_mod_box == NULL)
        return;

    /* 先算签名：只有插槽/内存条信息变化时才重建这几行 */
    GString *sig = g_string_new(NULL);

    g_string_append_printf(sig, "%llu|%u|%u;",
                           (unsigned long long)snap->probe_valid,
                           snap->mem_slot_total, snap->mem_module_count);

    for (uint32_t i = 0; i < snap->mem_module_count; i++) {
        const FmonMemModule *m = &snap->mem_modules[i];

        g_string_append_printf(sig, "%s|%u|%s|%u|%u|%s;", m->locator,
                               m->size_mb, m->type, m->speed_mts,
                               m->configured_mts, m->manufacturer);
    }

    if (g_strcmp0(sig->str, s_mod_sig) == 0) {
        g_string_free(sig, TRUE);
        return;
    }

    g_free(s_mod_sig);
    s_mod_sig = g_string_free(sig, FALSE);

    GtkWidget *child = NULL;

    while ((child = gtk_widget_get_first_child(s_mod_box)) != NULL)
        gtk_box_remove(GTK_BOX(s_mod_box), child);

    if ((snap->probe_valid & FMON_PROBE_MEM_MODULES) == 0) {
        gtk_box_append(GTK_BOX(s_mod_box),
                       mod_line(_("内存条信息不可用（dmidecode 未安装或探测失败）"),
                                "group-desc", FALSE));
        return;
    }

    if (snap->mem_module_count == 0) {
        gtk_box_append(GTK_BOX(s_mod_box),
                       mod_line(_("未检测到已安装的内存条"), "group-desc",
                                FALSE));
        return;
    }

    char b[96];

    g_snprintf(b, sizeof(b), "%u 个插槽 / %u 条已安装",
               snap->mem_slot_total, snap->mem_module_count);
    gtk_box_append(GTK_BOX(s_mod_box), mod_line(b, "hw-model", FALSE));

    for (uint32_t i = 0; i < snap->mem_module_count; i++) {
        const FmonMemModule *m = &snap->mem_modules[i];
        GString *line = g_string_new(NULL);
        unsigned size_mb = m->size_mb;

        /* 兼容旧守护进程：数值过小说明单位丢失，按 GB 处理 */
        if (size_mb > 0 && size_mb < 128)
            size_mb *= 1024;

        if (size_mb >= 1024)
            g_string_append_printf(line, "%u GB", size_mb / 1024);
        else
            g_string_append_printf(line, "%u MB", size_mb);

        if (m->type[0] != '\0')
            g_string_append_printf(line, " · %s", m->type);

        if (m->speed_mts > 0) {
            if (m->configured_mts > 0 && m->configured_mts != m->speed_mts)
                g_string_append_printf(line, " · %u MT/s（实际 %u）",
                                       m->speed_mts, m->configured_mts);
            else
                g_string_append_printf(line, " · %u MT/s", m->speed_mts);
        }

        if (m->manufacturer[0] != '\0')
            g_string_append_printf(line, " · %s", m->manufacturer);

        gchar *text = g_strdup_printf("%s  ·  %s",
                                      (m->locator[0] != '\0') ? m->locator
                                                               : _("内存条"),
                                      line->str);
        gtk_box_append(GTK_BOX(s_mod_box), mod_line(text, "hw-model", TRUE));
        g_free(text);
        g_string_free(line, TRUE);
    }
}


