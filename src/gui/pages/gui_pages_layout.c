/*
 * gui_pages_layout.c - 所有页面的创建与栈装配
 */

#include "gui_pages_layout.h"
#include "ui_metrics.h"

#include "gui_page_build.h"
#include "gui_page_state.h"
#include "gui_pages.h"
#include "gui_util.h"
#include "gui_page_net.h"
#include "gui_page_disk.h"
#include "gui_page_mem.h"
#include "gui_page_l2arc.h"
#include "gui_page_batt.h"
#include "gui_page_cpu.h"
#include "gui_page_zfs.h"
#include "gui_page_prune.h"
#include "gui_page_gpu.h"
#include "gui_coreview.h"
#include "gui_state.h"

#include "alert_view.h"
#include "history_view.h"
#include "log_view.h"
#include "network_view.h"
#include "process_view.h"
#include "rctl_view.h"
#include "service_view.h"
#include "storage_view.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

GtkStack *gui_pages_layout_build(GtkWidget *content)
{
    GtkStack *stack;
    const FmonShmData *snap = gui_state_snapshot();

    /* ---------- 卡片 ---------- */
    GtkWidget *page_storage = gui_storage_view_new();
    GtkWidget *page_proc    = gui_process_view_new();
    GtkWidget *page_svc     = gui_service_view_new();
    GtkWidget *page_conn    = gui_network_conn_view_new();
    GtkWidget *page_iface   = gui_network_iface_view_new();
    GtkWidget *page_log     = gui_log_view_new();
    GtkWidget *page_alerts  = gui_alert_view_new();
    GtkWidget *page_rctl    = gui_rctl_view_new();
    GtkWidget *page_history = gui_history_view_new();

    /* 概览分组下的独立性能页：大曲线 + 实时详情 */



    /* CPU 页：使用率由上方半圆仪表展示，这里只保留温度曲线 */
    g_cpu_chart.n = 0;
    g_cpu_chart.series[g_cpu_chart.n++] =
        (ComboSeries){ "温度", gui_metric_curve(12), "" };

    /* 运行/状态信息统一放进下方对齐网格，不再单独占底部卡片 */
    GtkWidget *page_cpu = gui_combo_page_new(
        "CPU 温度", "温度趋势（使用率见上方仪表）", &g_cpu_chart, NULL, 0,
        &g_cpu_page);

    /* 曲线卡片不再上屏：保留 combo 数据更新，界面用仪表 + 折线图卡片 */
    (void)page_cpu;

    /* 三张独立卡片：半圆仪表 / 负载与温度 / 线程与详情 */
    GtkWidget *cpu_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_3);
    GtkWidget *gauge = gui_cpu_gauge_card();

    /* CPU 型号放最上方仪表卡片右上角 */
    gtk_box_append(GTK_BOX(gui_cpu_gauge_header_right()),
                   gui_cpu_hw_header());

    /* 状态与调度移到仪表卡片右侧空白处 */
    GtkWidget *cpu_status = gui_cpu_status_box();

    gtk_widget_set_size_request(cpu_status, 420, -1);
    gtk_widget_set_valign(cpu_status, GTK_ALIGN_CENTER);
    gtk_flow_box_append(GTK_FLOW_BOX(gui_cpu_gauge_body()), cpu_status);

    gtk_box_append(GTK_BOX(cpu_root), gauge);
    gtk_box_append(GTK_BOX(cpu_root), gui_cpu_thermal_card());

    GtkWidget *cpu_details = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_2);
    gtk_widget_add_css_class(cpu_details, "page-card");

    g_core_caption = gtk_label_new(NULL);
    gtk_widget_add_css_class(g_core_caption, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(g_core_caption), 0.0f);
    gtk_widget_set_hexpand(g_core_caption, TRUE);

    g_core_area = gui_core_view_new();

    GtkWidget *core_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FMON_SPACE_2);
    gtk_widget_set_margin_start(core_box, FMON_SPACE_2);
    gtk_widget_set_margin_end(core_box, FMON_SPACE_2);
    gtk_box_append(GTK_BOX(core_box), g_core_caption);
    gtk_box_append(GTK_BOX(core_box), g_core_area);

    gtk_box_append(GTK_BOX(cpu_details), core_box);
    gtk_box_append(GTK_BOX(cpu_details), gui_cpu_detail_box());

    gtk_box_append(GTK_BOX(cpu_root), cpu_details);

    /* 内存 + Swap 合并页 */
    g_mem_chart.n = 0;
    g_mem_chart.series[g_mem_chart.n++] =
        (ComboSeries){ "内存使用率", gui_metric_curve(1), "" };
    g_mem_chart.series[g_mem_chart.n++] =
        (ComboSeries){ "Swap 使用率", gui_metric_curve(2), "" };

    GtkWidget *mp_mem = gui_combo_page_new(
        "内存", "物理内存与 Swap 使用率", &g_mem_chart, NULL, 0,
        &g_mem_page);

    /* 说明行放在标题下方（与副标题同一条左侧对齐），不再压到页面最底部。
     * ZFS ARC 占用的内存在压力下可回收，否则 88% 这类数字容易被误解。 */
    g_mem_note = gtk_label_new("");
    gtk_widget_add_css_class(g_mem_note, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(g_mem_note), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(g_mem_note), TRUE);

    {
        GtkWidget *title_box = gtk_widget_get_parent(g_mem_page.sub);

        if (GTK_IS_BOX(title_box))
            gtk_box_insert_child_after(GTK_BOX(title_box), g_mem_note,
                                       g_mem_page.sub);
        else
            gtk_box_append(GTK_BOX(mp_mem), g_mem_note);
    }

    /* 内存队列并进上面的大卡片（曲线下方），不再单独占一张卡 */
    GtkWidget *mem_big_card = gtk_widget_get_parent(g_mem_page.area);

    if (GTK_IS_BOX(mem_big_card))
        gtk_box_append(GTK_BOX(mem_big_card), gui_mem_queues_box());

    /* 内存条信息放到标题行右侧，不再单独占底部一张卡片 */
    gtk_box_append(GTK_BOX(g_mem_page.header_right),
                   gui_mem_modules_header());

    /* 内存/Swap 总容量放在标题行右侧，不再占底部卡片 */
    gtk_box_append(GTK_BOX(g_mem_page.header_right),
                   gui_mem_totals_box());

    /* 去掉底部卡片后，曲线区加高一点 */
    gtk_widget_set_size_request(g_mem_page.area, -1, 380);
    /* 网络：接收 / 发送 两条独立颜色曲线，数值显示在上方 */
    g_net_rx_cd.history     = snap->net_rx_history;
    g_net_rx_cd.time_us     = snap->history_time_us;
    g_net_rx_cd.unit        = "KB/s";
    g_net_rx_cd.max_val     = 0.0;
    g_net_rx_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    g_net_tx_cd.history     = snap->net_tx_history;
    g_net_tx_cd.time_us     = snap->history_time_us;
    g_net_tx_cd.unit        = "KB/s";
    g_net_tx_cd.max_val     = 0.0;
    g_net_tx_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    g_net_chart.n = 0;
    g_net_chart.series[g_net_chart.n++] = (ComboSeries){ "接收", &g_net_rx_cd, "" };
    g_net_chart.series[g_net_chart.n++] = (ComboSeries){ "发送", &g_net_tx_cd, "" };

    GtkWidget *mp_net = gui_combo_page_new("网络概览",
                                          "接收 / 发送（颜色区分，实时速率）",
                                          &g_net_chart, NULL, 0, &g_net_page);

    g_net_summary = gtk_label_new("");
    gtk_widget_add_css_class(g_net_summary, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(g_net_summary), 0.0f);

    {
        GtkWidget *card = gtk_widget_get_parent(g_net_page.sub);

        if (GTK_IS_BOX(card))
            gtk_box_insert_child_after(GTK_BOX(card), g_net_summary,
                                       g_net_page.sub);
    }

    /* 网卡型号单独占标题下的一整行，长型号可以完整换行显示。 */
    {
        GtkWidget *nic = gui_net_nic_header();
        GtkWidget *net_card = gtk_widget_get_parent(g_net_page.area);

        if (GTK_IS_BOX(net_card)) {
            GtkWidget *header = gtk_widget_get_first_child(net_card);

            if (header != NULL)
                gtk_box_insert_child_after(GTK_BOX(net_card), nic, header);
            else
                gtk_box_prepend(GTK_BOX(net_card), nic);
        }
    }

    /* 链路错误/丢包与 TCP 健康度并入网络大卡片 */
    {
        GtkWidget *net_card = gtk_widget_get_parent(g_net_page.area);

        if (GTK_IS_BOX(net_card))
            gtk_box_append(GTK_BOX(net_card), gui_net_health_box());
    }

    /* 接口详情已在“网络 → 网络接口”页，概览页不再重复同一张卡片 */

    /* 磁盘：读取 / 写入 两条曲线 + 繁忙/累计在标题下方 */
    g_disk_rd_cd.history     = snap->disk_read_history;
    g_disk_rd_cd.time_us     = snap->history_time_us;
    g_disk_rd_cd.unit        = "KB/s";
    g_disk_rd_cd.max_val     = 0.0;
    g_disk_rd_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    g_disk_wr_cd.history     = snap->disk_write_history;
    g_disk_wr_cd.time_us     = snap->history_time_us;
    g_disk_wr_cd.unit        = "KB/s";
    g_disk_wr_cd.max_val     = 0.0;
    g_disk_wr_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    g_disk_chart.n = 0;
    g_disk_chart.series[g_disk_chart.n++] =
        (ComboSeries){ "读取", &g_disk_rd_cd, "" };
    g_disk_chart.series[g_disk_chart.n++] =
        (ComboSeries){ "写入", &g_disk_wr_cd, "" };
    g_disk_chart.series[g_disk_chart.n++] =
        (ComboSeries){ "温度", gui_disk_temp_curve(), "" };

    GtkWidget *mp_disk = gui_combo_page_new(
        "磁盘", "读取 / 写入（所有磁盘合计）/ 温度", &g_disk_chart,
        NULL, 0, &g_disk_page);

    /* 三条带需要更多高度 */
    gtk_widget_set_size_request(g_disk_page.area, -1, 400);

    /* 每块磁盘的明细表（图表是合计） */
    {
        GtkWidget *card = gtk_widget_get_parent(g_disk_page.area);

        if (GTK_IS_BOX(card))
            gtk_box_append(GTK_BOX(card), gui_disk_table_box());
    }

    g_disk_summary = gtk_label_new("");
    gtk_widget_add_css_class(g_disk_summary, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(g_disk_summary), 0.0f);

    {
        GtkWidget *card = gtk_widget_get_parent(g_disk_page.sub);

        if (GTK_IS_BOX(card))
            gtk_box_insert_child_after(GTK_BOX(card), g_disk_summary,
                                       g_disk_page.sub);
    }

    /* 磁盘设备详情放到标题行右侧，不再单独占一张卡片 */
    gtk_box_append(GTK_BOX(g_disk_page.header_right),
                   gui_disk_details_header());
    gtk_box_append(GTK_BOX(g_disk_page.header_right),
                   gui_disk_perf_line());
    gtk_box_append(GTK_BOX(g_disk_page.header_right),
                   gui_disk_smart_line());
    static const char *const zread_keys[] = { "当前读速率", "累计读取",
                                              "累计读次数", "L2 读速率" };
    static const char *const zwrite_keys[] = { "当前写速率", "累计写入",
                                               "累计写次数", "L2 写速率" };
    static const char *const temp_keys[] = { "当前温度", "逻辑核心",
                                             "主机名", "系统版本" };
    static const char *const batt_keys[] = { "电量", "放电功率", "电源",
                                             "状态" };

    /* ZFS 池：使用率 + 错误趋势两条等宽曲线放同一张大卡片 */
    static ComboChart g_zpool_chart;

    g_zpool_cd.history     = snap->zpool_util_history;
    g_zpool_cd.time_us     = snap->history_time_us;
    g_zpool_cd.unit        = "%";
    g_zpool_cd.max_val     = 100.0;
    g_zpool_cd.interval_us = (uint64_t)FMON_SAMPLE_INTERVAL_MS * 1000ULL;

    g_zpool_chart.n = 0;
    g_zpool_chart.series[g_zpool_chart.n++] =
        (ComboSeries){ "使用率", &g_zpool_cd, "" };
    g_zpool_chart.series[g_zpool_chart.n++] =
        (ComboSeries){ "新增错误", gui_zpool_err_curve(), "" };

    GtkWidget *page_zpool = gui_combo_page_new(
        "ZFS 池空间", "使用率与错误趋势（颜色区分）", &g_zpool_chart, NULL, 0,
        &g_zpool_page);

    gtk_box_append(GTK_BOX(g_zpool_page.header_right),
                   gui_zpool_header_box());

    /* 池详情也放进同一张大卡片（曲线下方），不再单独悬在外面 */
    {
        GtkWidget *zcard = gtk_widget_get_parent(g_zpool_page.area);

        if (GTK_IS_BOX(zcard))
            gtk_box_append(GTK_BOX(zcard), gui_zpool_detail_box());
    }
    GtkWidget *mp_arc    = gui_metric_page_new(
        &g_metrics[7], 7, "ZFS ARC",
        "曲线：ARC 大小占上限百分比（与“占上限”一致）", NULL, 0,
        gui_upd_arc, TRUE);

    /* ARC 明细三列，放在曲线下方同一张卡里 */
    {
        GtkWidget *arc_card = gtk_widget_get_parent(g_metrics[7].area);

        if (GTK_IS_BOX(arc_card)) {
            GtkWidget *tt = gtk_label_new(_("构成趋势（数据缓冲 / 元数据）"));

            gtk_widget_add_css_class(tt, "detail-title");
            gtk_label_set_xalign(GTK_LABEL(tt), 0.0f);
            gtk_widget_set_margin_top(tt, FMON_SPACE_3);
            gtk_box_append(GTK_BOX(arc_card), tt);
            gtk_box_append(GTK_BOX(arc_card), gui_arc_trend_chart());
            gtk_box_append(GTK_BOX(arc_card), gui_arc_detail_box());
        }
    }
    GtkWidget *mp_zhit   = gui_metric_page_new(&g_metrics[8], 8, "ZFS 命中率",
                                              "分类型命中率与淘汰压力", NULL, 0,
                                              gui_upd_zhit, TRUE);

    /* 命中率明细三列，放在曲线下方 */
    {
        GtkWidget *zcard = gtk_widget_get_parent(g_metrics[8].area);

        if (GTK_IS_BOX(zcard)) {
            GtkWidget *rt = gtk_label_new(_("近 60 秒命中率"));

            gtk_widget_add_css_class(rt, "detail-title");
            gtk_label_set_xalign(GTK_LABEL(rt), 0.0f);
            gtk_widget_set_margin_top(rt, FMON_SPACE_3);
            gtk_box_append(GTK_BOX(zcard), rt);
            gtk_box_append(GTK_BOX(zcard), gui_zhit_roll_chart());
            gtk_box_append(GTK_BOX(zcard), gui_zhit_detail_box());
        }
    }
    GtkWidget *mp_zread  = gui_metric_page_new(&g_metrics[9], 9, "zpool 读带宽",
                                              "读速率与累计", zread_keys, 4,
                                              gui_upd_zread, TRUE);
    GtkWidget *mp_zwrite = gui_metric_page_new(&g_metrics[10], 10, "zpool 写带宽",
                                              "写速率与累计", zwrite_keys, 4,
                                              gui_upd_zwrite, TRUE);
    GtkWidget *mp_zprune = gui_metric_page_new(&g_metrics[11], 11, "ARC Prune",
                                              "累计回收次数（只增不减；无回收时保持 0）与幽灵缓存",
                                              NULL, 0, gui_upd_zprune, TRUE);

    {
        GtkWidget *pcard = gtk_widget_get_parent(g_metrics[11].area);

        if (GTK_IS_BOX(pcard)) {
            GtkWidget *gt = gtk_label_new(_("幽灵缓存趋势（MRU / MFU）"));

            gtk_widget_add_css_class(gt, "detail-title");
            gtk_label_set_xalign(GTK_LABEL(gt), 0.0f);
            gtk_widget_set_margin_top(gt, FMON_SPACE_3);
            gtk_box_append(GTK_BOX(pcard), gt);
            gtk_box_append(GTK_BOX(pcard), gui_prune_ghost_chart());
            gtk_box_append(GTK_BOX(pcard), gui_prune_detail_box());
        }
    }
    GtkWidget *mp_l2arc  = gui_l2arc_page_new();

    GtkWidget *mp_temp   = gui_metric_page_new(&g_metrics[12], 12, "CPU 温度",
                                              "温度与主机信息", temp_keys, 4,
                                              gui_upd_temp, FALSE);
    GtkWidget *mp_batt   = gui_metric_page_new(&g_metrics[13], 13, "电池",
                                              "电量、功率与电源", batt_keys, 4,
                                              gui_upd_batt, FALSE);

    /* 电池健康与供电细节放在同一张卡片下方 */
    {
        GtkWidget *bcard = gtk_widget_get_parent(g_metrics[13].area);

        if (GTK_IS_BOX(bcard))
            gtk_box_append(GTK_BOX(bcard), gui_batt_detail_box());
    }

    /* ZFS 合并页：4 条曲线同框 + 带宽 2 条曲线 */
    static const char *const zsum_rows[] = {
        "##池空间", "池名", "状态", "已用 / 总量", "使用率",
        "##ARC", "大小", "上限", "占内存",
        "##命中率", "命中率", "L2 命中 / 未命中", "幽灵命中",
        "##ARC Prune", "累计回收",
    };
    g_zsum_chart.n = 0;
    g_zsum_chart.series[g_zsum_chart.n++] =
        (ComboSeries){ "池空间", gui_metric_curve(6), "" };
    g_zsum_chart.series[g_zsum_chart.n++] =
        (ComboSeries){ "ARC", gui_metric_curve(7), "" };
    g_zsum_chart.series[g_zsum_chart.n++] =
        (ComboSeries){ "命中率", gui_metric_curve(8), "" };
    g_zsum_chart.series[g_zsum_chart.n++] =
        (ComboSeries){ "Prune", gui_metric_curve(11), "" };

    g_zbw_chart.n = 0;
    g_zbw_chart.series[g_zbw_chart.n++] =
        (ComboSeries){ "读取", gui_metric_curve(9), "" };
    g_zbw_chart.series[g_zbw_chart.n++] =
        (ComboSeries){ "写入", gui_metric_curve(10), "" };

    GtkWidget *page_zsum = gui_combo_page_new(
        "ZFS 总览", "池空间 / ARC / 命中率 / Prune（颜色区分）",
        &g_zsum_chart, zsum_rows, (int)G_N_ELEMENTS(zsum_rows), &g_zsum_page);
    /* 数值直接画在各自曲线带内（见图表第二行），不再占底部卡片 */
    GtkWidget *page_zbw = gui_combo_page_new(
        "ZFS 带宽", "读取 / 写入带宽（所有池合计；空闲时为 0）",
        &g_zbw_chart, NULL, 0, &g_zbw_page);

    /* 累计/操作/队列/延迟直方图放到曲线下方 */
    {
        GtkWidget *bcard = gtk_widget_get_parent(g_zbw_page.area);

        if (GTK_IS_BOX(bcard))
            gtk_box_append(GTK_BOX(bcard), gui_zbw_detail_box());
    }

    gui_net_details_update();
    gui_disk_details_update();
    gui_gpu_pages_update();
    gui_mem_modules_update();


    stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
    gtk_stack_add_named(stack, page_storage, "storage");
    gtk_stack_add_named(stack, page_proc, "proc");
    gtk_stack_add_named(stack, page_svc, "svc");
    gtk_stack_add_named(stack, page_conn, "conn");
    gtk_stack_add_named(stack, page_iface, "iface");
    gtk_stack_add_named(stack, page_log, "log");
    gtk_stack_add_named(stack, page_alerts, "alerts");
    gtk_stack_add_named(stack, page_rctl, "rctl");
    gtk_stack_add_named(stack, page_history, "history");

    gtk_stack_add_named(stack, gui_page_scrolled(cpu_root), "cpu");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_mem), "mem");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_net), "netm");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_disk), "disk");

    gtk_stack_add_named(stack, gui_page_scrolled(page_zpool), "zpool");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_arc), "arc");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_zhit), "zhit");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_zread), "zread");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_zwrite), "zwrite");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_zprune), "zprune");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_l2arc), "l2arc");
    static const char *gpu_pages[FMON_MAX_GPUS] = { "gpu0", "gpu1", "gpu2",
                                                     "gpu3" };

    for (int gi = 0; gi < FMON_MAX_GPUS; gi++)
        gtk_stack_add_named(stack, gui_page_scrolled(gui_gpu_page_new(gi)),
                            gpu_pages[gi]);

    gtk_stack_add_named(stack, gui_page_scrolled(mp_temp), "temp");
    gtk_stack_add_named(stack, gui_page_scrolled(mp_batt), "batt");

    gtk_stack_add_named(stack, gui_page_scrolled(page_zsum), "zsum");
    gtk_stack_add_named(stack, gui_page_scrolled(page_zbw), "zbw");

    gtk_box_append(GTK_BOX(content), GTK_WIDGET(stack));

    return stack;
}
