/*
 * gui_page_gpu_detail.c - GPU 页面详情卡片
 */

#include "gui_page_gpu.h"

#include "gui_state.h"
#include "gui_util.h"
#include "gui_widgets.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>

static const FmonShmData *snap(void)
{
    return gui_state_snapshot();
}

static void gpu_row(GtkWidget *grid, int *row, const char *key,
                    const char *value)
{
    gtk_grid_attach(GTK_GRID(grid), gui_make_cell(key, "detail-key"), 0, *row, 1,
                    1);
    gtk_grid_attach(GTK_GRID(grid), gui_make_cell(value, NULL), 1, *row, 1, 1);
    (*row)++;
}

void gui_gpu_fill_details(GtkWidget *grid, int slot)
{
    if (grid == NULL)
        return;

    GString *sig = g_string_new(NULL);

    if (slot >= 0 && slot < (int)snap()->gpu_count) {
        const FmonGpuInfo *gg = &snap()->gpus[slot];

        g_string_append_printf(
            sig, "%s|%s|%s|%s|%.0f|%.0f|%.1f|%.1f|%.1f|%.1f|%.1f|%.1f|"
                 "%.0f|%.0f|%.0f|%.0f|%.2f|%.2f|%.0f|%u|%u|%.0f|%.0f|"
                 "%.1f|%.1f|%u|%llu|%llu|%u|%u|%s;",
            gg->name, gg->driver_version, gg->vbios_version, gg->uuid,
            gg->mem_used_mb, gg->mem_total_mb, gg->usage_pct,
            gg->util_mem_pct, gg->util_enc_pct, gg->util_dec_pct,
            gg->util_jpeg_pct, gg->util_ofa_pct, (double)gg->clock_mhz,
            gg->clock_sm_mhz, gg->clock_mem_mhz, gg->clock_video_mhz,
            gg->power_w, gg->power_limit_w, gg->fan_pct, gg->pcie_gen,
            gg->pcie_width, gg->pcie_rx_mbps, gg->pcie_tx_mbps,
            gg->bar1_used_mb, gg->bar1_total_mb, gg->ecc_mode,
            (unsigned long long)gg->ecc_sbe,
            (unsigned long long)gg->ecc_dbe, gg->pstate, gg->sm_count,
            gg->serial);
    }

    gboolean changed = gui_grid_signature_changed(grid, sig->str);

    g_string_free(sig, TRUE);

    if (!changed)
        return;

    gui_grid_clear(grid);

    if (slot < 0 || slot >= (int)snap()->gpu_count) {
        gtk_grid_attach(GTK_GRID(grid),
                        gui_make_cell("未检测到 GPU", "detail-key"), 0, 0, 2, 1);
        return;
    }

    const FmonGpuInfo *g = &snap()->gpus[slot];
    char  b[160];
    int   row = 0;

    bool nv = (strcmp(g->vendor, "NVIDIA") == 0);

    if (g->driver_version[0] != '\0')
        gpu_row(grid, &row, "驱动版本", g->driver_version);
    if (g->vbios_version[0] != '\0')
        gpu_row(grid, &row, "固件 (VBIOS)", g->vbios_version);
    if (g->serial[0] != '\0')
        gpu_row(grid, &row, "序列号", g->serial);
    if (g->uuid[0] != '\0')
        gpu_row(grid, &row, "UUID", g->uuid);

    if (g->kind == FMON_GPU_INTEGRATED) {
        /* 核显：共享内存（GTT）与专用显存（VRAM）分开显示。
         * 旧守护进程把两者字段对调过，按“VRAM 一定更小”做一次兼容纠正。 */
        double vram_used  = g->mem_used_mb;
        double vram_total = g->mem_total_mb;
        double gtt_used   = g->gtt_used_mb;
        double gtt_total  = g->gtt_total_mb;

        if (vram_total > gtt_total) {
            vram_used  = g->gtt_used_mb;
            vram_total = g->gtt_total_mb;
            gtt_used   = g->mem_used_mb;
            gtt_total  = g->mem_total_mb;
        }

        if (gtt_total > 0.0) {
            g_snprintf(b, sizeof(b), "%.0f / %.0f MB（共享系统内存）",
                       gtt_used, gtt_total);
            gpu_row(grid, &row, "共享内存 (GTT)", b);
        }

        if (vram_total > 0.0) {
            g_snprintf(b, sizeof(b), "%.0f / %.0f MB（专用）",
                       vram_used, vram_total);
            gpu_row(grid, &row, "专用显存 (VRAM)", b);
        }
    } else {
        g_snprintf(b, sizeof(b), "%.0f / %.0f MB（空闲 %.0f MB）",
                   g->mem_used_mb, g->mem_total_mb,
                   (g->mem_total_mb > g->mem_used_mb)
                       ? g->mem_total_mb - g->mem_used_mb
                       : 0.0);
        gpu_row(grid, &row, "显存 (VRAM)", b);
    }

    if (nv) {
        g_snprintf(b, sizeof(b), "GPU %.1f%% · 显存 %.1f%% · 编码 %.1f%% · "
                                 "解码 %.1f%% · JPEG %.1f%% · OFA %.1f%%",
                   g->usage_pct, g->util_mem_pct, g->util_enc_pct,
                   g->util_dec_pct, g->util_jpeg_pct, g->util_ofa_pct);
        gpu_row(grid, &row, "利用率细分", b);

        g_snprintf(b, sizeof(b), "图形 %.0f MHz · SM %.0f MHz · 显存 %.0f MHz · "
                                 "视频 %.0f MHz",
                   g->clock_sm_mhz, g->clock_sm_mhz, g->clock_mem_mhz,
                   g->clock_video_mhz);
        /* 图形时钟用 clock_mhz，SM 用 clock_sm_mhz */
        {
            GString *clk = g_string_new(NULL);

            if (g->clock_mhz > 0)
                g_string_append_printf(clk, "图形 %.0f MHz",
                                       (double)g->clock_mhz);
            if (g->clock_sm_mhz > 0)
                g_string_append_printf(clk, "%sSM %.0f MHz",
                                       (clk->len > 0) ? " · " : "",
                                       g->clock_sm_mhz);
            if (g->clock_mem_mhz > 0)
                g_string_append_printf(clk, "%s显存 %.0f MHz",
                                       (clk->len > 0) ? " · " : "",
                                       g->clock_mem_mhz);
            if (g->clock_video_mhz > 0)
                g_string_append_printf(clk, "%s视频 %.0f MHz",
                                       (clk->len > 0) ? " · " : "",
                                       g->clock_video_mhz);

            if (clk->len > 0)
                gpu_row(grid, &row, "时钟频率", clk->str);
            g_string_free(clk, TRUE);
        }

        if (g->power_w >= 0.0) {
            if (g->power_limit_w > 0.0)
                g_snprintf(b, sizeof(b), "%.2f W / 上限 %.2f W", g->power_w,
                           g->power_limit_w);
            else
                g_snprintf(b, sizeof(b), "%.2f W（未报告上限）", g->power_w);
        } else {
            g_strlcpy(b, "—", sizeof(b));
        }
        gpu_row(grid, &row, "功耗", b);

        if (g->fan_pct >= 0.0)
            g_snprintf(b, sizeof(b), "%.0f %%", g->fan_pct);
        else
            g_strlcpy(b, "不可用（没有转速传感器）", sizeof(b));
        gpu_row(grid, &row, "风扇转速", b);

        if (g->pcie_gen > 0) {
            g_snprintf(b, sizeof(b), "Gen%u × %u · 吞吐 收 %.1f / 发 %.1f MB/s",
                       g->pcie_gen, g->pcie_width, g->pcie_rx_mbps,
                       g->pcie_tx_mbps);
            gpu_row(grid, &row, "PCIe", b);
        }

        if (g->bar1_total_mb > 0.0) {
            g_snprintf(b, sizeof(b), "已用 %.0f / 总 %.0f MB（空闲 %.0f MB）",
                       g->bar1_used_mb, g->bar1_total_mb,
                       g->bar1_total_mb - g->bar1_used_mb);
            gpu_row(grid, &row, "BAR1 显存映射", b);
        }

        if (g->ecc_mode != 0) {
            const char *ecc = (g->ecc_mode == 2) ? "已开启" : "已关闭";

            g_snprintf(b, sizeof(b), "%s（单比特 %llu / 双比特 %llu）", ecc,
                       (unsigned long long)g->ecc_sbe,
                       (unsigned long long)g->ecc_dbe);
            gpu_row(grid, &row, "ECC 错误", b);
        }

        if (g->sm_count > 0)
            g_snprintf(b, sizeof(b), "P%u · 计算核心 %u", g->pstate,
                       g->sm_count);
        else
            g_snprintf(b, sizeof(b), "P%u", g->pstate);
        gpu_row(grid, &row, "运行状态", b);
    } else {
        /* AMD：温度来自 libdrm 传感器；风扇/功耗多数驱动不暴露 */
        if (g->temp_c >= 0.0)
            g_snprintf(b, sizeof(b), "%.1f°C", g->temp_c);
        else
            g_strlcpy(b, "不可用（驱动未暴露）", sizeof(b));
        gpu_row(grid, &row, "GPU 温度", b);

        if (g->power_w > 0.0 && g->power_limit_w > 0.0)
            g_snprintf(b, sizeof(b), "%.2f W / 上限 %.2f W", g->power_w,
                       g->power_limit_w);
        else if (g->power_w > 0.0)
            g_snprintf(b, sizeof(b), "%.2f W", g->power_w);
        else
            g_strlcpy(b, "0.00 W（核显通常不单独计量）", sizeof(b));
        gpu_row(grid, &row, "功耗", b);

        if (g->fan_pct > 0.0)
            g_snprintf(b, sizeof(b), "%.0f %%", g->fan_pct);
        else
            g_strlcpy(b,
                     "0 %（核显无独立风扇，整机风扇由 EC 控制）",
                     sizeof(b));
        gpu_row(grid, &row, "风扇转速", b);

        if (g->clock_mhz > 0) {
            g_snprintf(b, sizeof(b), "%.0f MHz", (double)g->clock_mhz);
            gpu_row(grid, &row, "图形时钟", b);
        }

        if (g->clock_mem_mhz > 0) {
            g_snprintf(b, sizeof(b), "%.0f MHz", g->clock_mem_mhz);
            gpu_row(grid, &row, "显存时钟", b);
        }

        if (g->usage_pct >= 0.0) {
            GString *us = g_string_new(NULL);

            g_string_append_printf(us, "GPU %.1f%%", g->usage_pct);

            if (g->gtt_total_mb > 0.0)
                g_string_append_printf(us, " · 共享内存 %.1f%%",
                                       100.0 * g->gtt_used_mb /
                                           g->gtt_total_mb);
            if (g->mem_total_mb > 0.0)
                g_string_append_printf(us, " · 专用显存 %.1f%%",
                                       100.0 * g->mem_used_mb /
                                           g->mem_total_mb);

            gpu_row(grid, &row, "利用率细分", us->str);
            g_string_free(us, TRUE);
        }

        if (g->pcie_gen > 0) {
            g_snprintf(b, sizeof(b), "Gen%u × %u", g->pcie_gen,
                       g->pcie_width);
            gpu_row(grid, &row, "PCIe", b);
        }
    }
}
