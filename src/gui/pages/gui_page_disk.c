/*
 * gui_page_disk.c - 磁盘设备详情卡片与刷新
 *
 * 由 main.c 拆出：数据来源于 gui_state_snapshot() 的一致快照。
 */

#include "gui_page_disk.h"
#include "ui_metrics.h"

#include "disk_perf.h"
#include "fmon_child.h"
#include "fmon_smart.h"
#include "alerts.h"
#include "gui_state.h"
#include "gui_util.h"
#include "gui_widgets.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>

static GtkWidget *s_box = NULL;   /* 标题行右侧的磁盘摘要 */
static gchar     *s_sig = NULL;

static GtkWidget *disk_line(const char *text, const char *css, gboolean dim)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    if (dim)
        gtk_widget_add_css_class(l, "group-desc");

    gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_RIGHT);
    return l;
}

GtkWidget *gui_disk_details_header(void)
{
    s_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);
    gtk_widget_set_halign(s_box, GTK_ALIGN_END);
    gtk_widget_set_valign(s_box, GTK_ALIGN_START);
    return s_box;
}

static double    s_temp_hist[FMON_HISTORY_POINTS];
static uint64_t  s_temp_time[FMON_HISTORY_POINTS];
static CurveData s_temp_cd = {
    .history = s_temp_hist,
    .time_us = s_temp_time,
    .unit    = "°C",
    .r = 1.00, .g = 0.00, .b = 0.00,
};

CurveData *gui_disk_temp_curve(void)
{
    s_temp_cd.interval_us    = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;
    s_temp_cd.alert_settings = alerts_current();
    s_temp_cd.alert_metric   = 12;   /* 温度阈值 */
    return &s_temp_cd;
}

void gui_disk_temp_sample(void)
{
    if (s_temp_cd.frozen)
        return;

    const FmonShmData *s = gui_state_snapshot();
    const FmonDiskInfo *src = NULL;

    if (s != NULL) {
        for (uint32_t i = 0; i < s->disk_count && i < FMON_MAX_DISKS; i++) {
            if (s->disks[i].smart_valid != 0 && s->disks[i].temp_c > 0.0) {
                src = &s->disks[i];
                break;
            }
        }
    }

    if (src == NULL)
        return;   /* 没有 SMART 就不画，避免一条 0 线误导 */

    gint64 now = g_get_monotonic_time();

    s_temp_hist[s_temp_cd.index] = src->temp_c;
    s_temp_time[s_temp_cd.index] = (uint64_t)now;
    s_temp_cd.index = (s_temp_cd.index + 1) % FMON_HISTORY_POINTS;

    if (s_temp_cd.count < FMON_HISTORY_POINTS)
        s_temp_cd.count++;

    s_temp_cd.end_time_us = (uint64_t)now;
}

static GtkWidget *s_perf_line = NULL;

GtkWidget *gui_disk_perf_line(void)
{
    s_perf_line = gtk_label_new("");
    gtk_widget_add_css_class(s_perf_line, "hw-model");
    gtk_label_set_xalign(GTK_LABEL(s_perf_line), 1.0f);
    gtk_widget_set_halign(s_perf_line, GTK_ALIGN_END);
    gtk_widget_set_margin_top(s_perf_line, FMON_SPACE_2);
    return s_perf_line;
}

void gui_disk_perf_update(void)
{
    if (s_perf_line == NULL)
        return;

    const DiskPerf *p = disk_perf_get();

    if (p == NULL || !p->valid) {
        gtk_label_set_text(GTK_LABEL(s_perf_line), _("性能：采集中…"));
        return;
    }

    gchar *txt = g_strdup_printf(
        _("IOPS 读 %.0f / 写 %.0f · 延迟 %.2f ms · 繁忙 %.0f%% · 队列 %.1f"),
        p->read_iops, p->write_iops, p->latency_ms, p->busy_pct,
        p->queue_len);

    gtk_label_set_text(GTK_LABEL(s_perf_line), txt);
    g_free(txt);
}

static GtkWidget *s_smart_line = NULL;

GtkWidget *gui_disk_smart_line(void)
{
    s_smart_line = gtk_label_new("");
    gtk_widget_add_css_class(s_smart_line, "hw-model");
    gtk_label_set_xalign(GTK_LABEL(s_smart_line), 1.0f);
    gtk_widget_set_halign(s_smart_line, GTK_ALIGN_END);
    gtk_widget_set_margin_top(s_smart_line, FMON_SPACE_1);
    return s_smart_line;
}

void gui_disk_smart_update(void)
{
    if (s_smart_line == NULL)
        return;

    const DiskPerf *p = disk_perf_get();
    const FmonShmData *s = gui_state_snapshot();
    const FmonDiskInfo *smart = NULL;

    if (p == NULL || !p->valid) {
        gtk_label_set_text(GTK_LABEL(s_smart_line), "");
        return;
    }

    if (s != NULL) {
        for (uint32_t i = 0; i < s->disk_count && i < FMON_MAX_DISKS; i++) {
            if (s->disks[i].smart_valid != 0) {
                smart = &s->disks[i];
                break;
            }
        }
    }

    char *rd = fmon_gui_format_bytes(p->total_read_bytes);
    char *wr = fmon_gui_format_bytes(p->total_write_bytes);

    gchar *txt;

    /* 温度只在上方“温度”曲线带显示，这里不再重复 */
    if (smart != NULL && smart->wear_pct >= 0 && smart->spare_pct >= 0) {
        txt = g_strdup_printf(_("寿命已用 %d%% · 备用空间 %d%% · 累计 读 %s / 写 %s"),
                              smart->wear_pct, smart->spare_pct, rd, wr);
    } else {
        txt = g_strdup_printf(_("累计 读 %s / 写 %s"), rd, wr);
    }

    gtk_label_set_text(GTK_LABEL(s_smart_line), txt);
    g_free(txt);
    g_free(rd);
    g_free(wr);
}

/* 从 geom disk list 补充接口/扇区/链路信息（普通用户可读，仅磁盘变化时调用） */
static void geom_extras(const char *name, char *iface, size_t ilen,
                        char *link, size_t llen, unsigned *sector)
{
    iface[0] = '\0';
    link[0]  = '\0';
    *sector  = 0;

    const char *const argv[] = { "/sbin/geom", "disk", "list", name, NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 64 * 1024, 2000, &res) == 0 &&
        fmon_child_ok(&res) && res.out != NULL) {
        char *save = NULL;

        for (char *line = strtok_r(res.out, "\n", &save); line != NULL;
             line = strtok_r(NULL, "\n", &save)) {
            char *p = line;

            while (*p == ' ' || *p == '\t')
                p++;

            if (strncmp(p, "Sectorsize:", 11) == 0) {
                *sector = (unsigned)strtoul(p + 11, NULL, 10);
            } else if (strncmp(p, "lunname:", 8) == 0) {
                char *v = p + 8;

                while (*v == ' ' || *v == '\t')
                    v++;

                char *e = v + strlen(v);

                while (e > v && (e[-1] == '\n' || e[-1] == '\r' ||
                                 e[-1] == ' ' || e[-1] == '\t'))
                    *--e = '\0';

                g_strlcpy(link, v, llen);
            }
        }
    }

    fmon_child_result_free(&res);

    if (strncmp(name, "nda", 3) == 0)
        g_strlcpy(iface, _("NVMe"), ilen);
    else if (strncmp(name, "ada", 3) == 0)
        g_strlcpy(iface, _("SATA"), ilen);
    else if (strstr(link, "USB") != NULL)
        g_strlcpy(iface, _("USB"), ilen);
    else if (link[0] != '\0')
        g_strlcpy(iface, _("SCSI/SAS"), ilen);
    else
        g_strlcpy(iface, _("未知接口"), ilen);
}

/* 每块磁盘的明细表：速率 / 累计 / 繁忙，图表是合计，这里看单盘 */
#define DISK_TAB_COLS 7

static GtkWidget *s_tab_val[FMON_MAX_DISKS][DISK_TAB_COLS];

static void tab_cell(GtkWidget *grid, int row, int col, const char *text,
                     gboolean head)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_widget_add_css_class(l, head ? "detail-key" : "detail-value");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_set_size_request(l, (col == 0) ? 96 : 118, -1);
    gtk_grid_attach(GTK_GRID(grid), l, col, row, 1, 1);
}

GtkWidget *gui_disk_table_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_1);

    gtk_widget_set_margin_top(box, FMON_SPACE_3);

    GtkWidget *t = gtk_label_new(_("磁盘明细（图表为所有磁盘合计）"));

    gtk_widget_add_css_class(t, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(box), t);

    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_row_spacing(GTK_GRID(grid), FMON_SPACE_1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), FMON_SPACE_3);

    static const char *const H[] = { "设备", "读取", "写入", "累计读",
                                     "累计写", "温度", "繁忙" };

    for (int c = 0; c < DISK_TAB_COLS; c++)
        tab_cell(grid, 0, c, H[c], TRUE);

    for (int i = 0; i < FMON_MAX_DISKS; i++) {
        for (int c = 0; c < DISK_TAB_COLS; c++) {
            char b[32];

            snprintf(b, sizeof(b), "—");
            s_tab_val[i][c] = gtk_label_new(b);
            gtk_widget_add_css_class(s_tab_val[i][c], "detail-value");
            gtk_label_set_xalign(GTK_LABEL(s_tab_val[i][c]), 0.0f);
            gtk_widget_set_size_request(s_tab_val[i][c], (c == 0) ? 96 : 118,
                                        -1);
            gtk_grid_attach(GTK_GRID(grid), s_tab_val[i][c], c, i + 1, 1, 1);
        }
    }

    gtk_box_append(GTK_BOX(box), grid);
    return box;
}

static const char *smart_source_label(uint32_t src)
{
    switch (src) {
    case FMON_SMART_SRC_NVME_IOCTL:
        return "NVMe ioctl";
    case FMON_SMART_SRC_ATA_CAM:
        return "ATA CAM";
    case FMON_SMART_SRC_SCSI_CAM:
        return "SCSI CAM";
    case FMON_SMART_SRC_NVMECONTROL:
        return "nvmecontrol 回退";
    case FMON_SMART_SRC_SMARTCTL:
        return "smartctl 回退";
    default:
        return "不可用";
    }
}

static void fmt_rate(char *b, size_t n, double kbps)
{
    if (kbps >= 1024.0)
        g_snprintf(b, n, "%.2f MB/s", kbps / 1024.0);
    else
        g_snprintf(b, n, "%.1f KB/s", kbps);
}

void gui_disk_table_update(void)
{
    const FmonShmData *s = gui_state_snapshot();

    if (s_tab_val[0][0] == NULL ||
        !gtk_widget_get_mapped(s_tab_val[0][0]))
        return;

    for (int i = 0; i < FMON_MAX_DISKS; i++) {
        gboolean on = (i < (int)s->disk_count);

        for (int c = 0; c < DISK_TAB_COLS; c++)
            gtk_widget_set_visible(s_tab_val[i][c], on);

        if (!on)
            continue;

        const FmonDiskInfo *d = &s->disks[i];
        char b[64];

        gtk_label_set_text(GTK_LABEL(s_tab_val[i][0]), d->name);

        fmt_rate(b, sizeof(b), d->read_kbps);
        gtk_label_set_text(GTK_LABEL(s_tab_val[i][1]), b);

        fmt_rate(b, sizeof(b), d->write_kbps);
        gtk_label_set_text(GTK_LABEL(s_tab_val[i][2]), b);

        char *r = fmon_gui_format_bytes(d->read_bytes);
        char *w = fmon_gui_format_bytes(d->write_bytes);

        gtk_label_set_text(GTK_LABEL(s_tab_val[i][3]), r);
        gtk_label_set_text(GTK_LABEL(s_tab_val[i][4]), w);
        g_free(r);
        g_free(w);

        if (d->smart_valid != 0 && d->temp_c > 0.0)
            g_snprintf(b, sizeof(b), "%.1f°C", d->temp_c);
        else
            g_snprintf(b, sizeof(b), "—");
        gtk_label_set_text(GTK_LABEL(s_tab_val[i][5]), b);

        char tip[96];

        g_snprintf(tip, sizeof(tip), "SMART 来源：%s",
                   smart_source_label(d->smart_source));
        gtk_widget_set_tooltip_text(s_tab_val[i][5], tip);

        g_snprintf(b, sizeof(b), "%.0f%%", d->busy_pct);
        gtk_label_set_text(GTK_LABEL(s_tab_val[i][6]), b);
    }
}

void gui_disk_details_update(void)
{
    const FmonShmData *snap = gui_state_snapshot();

    if (s_box == NULL || !gtk_widget_get_mapped(s_box))
        return;

    GString *sig = g_string_new(NULL);

    for (uint32_t i = 0; i < snap->disk_count && i < FMON_MAX_DISKS; i++) {
        const FmonDiskInfo *d = &snap->disks[i];

        g_string_append_printf(sig, "%s|%s|%llu|%u|%s|%u;", d->name, d->model,
                               (unsigned long long)d->size_bytes, d->kind,
                               d->serial, d->meta_valid);
    }

    /* 这里只展示设备固有信息；读写速率/繁忙度在曲线上，不必进签名 */
    if (g_strcmp0(sig->str, s_sig) == 0) {
        g_string_free(sig, TRUE);
        return;
    }

    g_free(s_sig);
    s_sig = g_string_free(sig, FALSE);

    GtkWidget *child = NULL;

    while ((child = gtk_widget_get_first_child(s_box)) != NULL)
        gtk_box_remove(GTK_BOX(s_box), child);

    if (snap->disk_count == 0) {
        gtk_box_append(GTK_BOX(s_box),
                       disk_line(_("未检测到磁盘"), "group-desc", FALSE));
        return;
    }

    for (uint32_t i = 0; i < snap->disk_count && i < FMON_MAX_DISKS; i++) {
        const FmonDiskInfo *d = &snap->disks[i];
        GString *line = g_string_new(NULL);

        g_string_append_printf(line, "%s  ·  ", d->name);

        if (d->meta_valid == 0)
            g_string_append(line, _("元数据不可用"));
        else if (d->model[0] != '\0')
            g_string_append(line, d->model);
        else
            g_string_append(line, _("未知型号"));

        char *size = fmon_gui_format_bytes(d->size_bytes);

        g_string_append_printf(line, " · %s", size);
        g_free(size);

        uint32_t dtype = d->kind & FMON_DISK_TYPE_MASK;

        if (dtype == FMON_DISK_HDD)
            g_string_append(line, _(" · 机械硬盘"));
        else if (dtype == FMON_DISK_SSD)
            g_string_append(line, _(" · 固态硬盘"));
        else if (dtype == FMON_DISK_FLASH)
            g_string_append(line, _(" · 移动闪存盘"));
        else if (strncmp(d->name, "da", 2) == 0)
            g_string_append(line, _(" · 外置硬盘"));
        else
            g_string_append(line, _(" · 类型未知"));

        gtk_box_append(GTK_BOX(s_box), disk_line(line->str, "hw-model", FALSE));
        g_string_free(line, TRUE);

        if (d->meta_valid != 0 && d->serial[0] != '\0') {
            gchar *sn = g_strdup_printf(_("SN %s"), d->serial);

            gtk_box_append(GTK_BOX(s_box), disk_line(sn, "hw-model", TRUE));
            g_free(sn);
        }

        {
            char     iface[32], link[64];
            unsigned sector = 0;

            geom_extras(d->name, iface, sizeof(iface), link, sizeof(link),
                        &sector);

            uint32_t ib = d->kind & FMON_DISK_IFACE_MASK;

            if (ib == FMON_DISK_IFACE_NVME)
                g_strlcpy(iface, _("NVMe"), sizeof(iface));
            else if (ib == FMON_DISK_IFACE_SATA)
                g_strlcpy(iface, _("SATA"), sizeof(iface));
            else if (ib == FMON_DISK_IFACE_USB) {
                uint32_t sp = d->kind & FMON_DISK_USB_SPEED_MASK;

                if (sp == FMON_DISK_USB3)
                    g_strlcpy(iface, _("USB 链路 3.0（5 Gbps）"), sizeof(iface));
                else if (sp == FMON_DISK_USB2)
                    g_strlcpy(iface, _("USB 链路 2.0（480 Mbps）"), sizeof(iface));
                else
                    g_strlcpy(iface, _("USB 链路"), sizeof(iface));
            }
            else if (ib == FMON_DISK_IFACE_SCSI)
                g_strlcpy(iface, _("SCSI/SAS"), sizeof(iface));

            uint32_t tr = d->kind & FMON_DISK_TRANSPORT_MASK;
            const char *ts = "";

            if (tr == FMON_DISK_TS_SATA3)
                ts = _("SATA 6 Gb/s");
            else if (tr == FMON_DISK_TS_SATA2)
                ts = _("SATA 3 Gb/s");
            else if (tr == FMON_DISK_TS_SATA1)
                ts = _("SATA 1.5 Gb/s");
            else if (tr == FMON_DISK_TS_NVME)
                ts = _("PCIe/NVMe");

            GString *ex = g_string_new(NULL);

            g_string_append_printf(ex, _("接口 %s"), iface);

            if (link[0] != '\0')
                g_string_append_printf(ex, "（%s）", link);

            if (ts[0] != '\0')
                g_string_append_printf(ex, _(" · 磁盘 %s"), ts);

            if (sector > 0)
                g_string_append_printf(ex, _(" · 扇区 %u B"), sector);

            GtkWidget *exl = disk_line(ex->str, "hw-model", TRUE);

            gtk_widget_set_tooltip_text(
                exl,
                _("USB 链路速率是设备与主机实际协商的结果。若设备或线材是\n"
                  "USB 2.0，即使插在 USB 3.0 接口上也只能协商到 480 Mbps；\n"
                  "物理接口版本无法由软件直接读取。\n"
                  "磁盘一行显示硬盘本身的 SATA/NVMe 接口能力。"));

            gtk_box_append(GTK_BOX(s_box), exl);
            g_string_free(ex, TRUE);
        }
    }
}


/* ===========================================================================
 * 窗口构建
 * ========================================================================= */
