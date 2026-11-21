/*
 * gui_export.c - 监控数据导出（CSV）
 *
 * 依赖一致快照与主窗口；保存对话框使用 GtkFileDialog 异步接口。
 */

#include "gui_export.h"

#include "gui_state.h"
#include "gui_util.h"
#include "history/history_provider.h"
#include "history/long_history.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern GtkWindow *g_main_window;

static void csv_write_value(FILE *fp, double v, const char *fmt)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];

    /* NaN/Inf 表示这个采样点没有有效数据：CSV 留空列，
     * 不要导出 "nan" 让 Excel/脚本把它当成数值。 */
    if (!isfinite(v))
        return;

    /* g_ascii_formatd 保证小数点永远是 '.'，不受用户 locale 影响
     * （旧版本在 de_DE / fr_FR 等 locale 下会导出 "1,23" 破坏 CSV） */
    g_ascii_formatd(buf, sizeof(buf), fmt, v);
    fputs(buf, fp);
}

static guint write_long_history_csv(FILE *fp)
{
    const uint64_t *times = fmon_history_time_us();
    uint32_t        n = fmon_history_count();
    const FmonHistorySeries *cpu = fmon_history_series(FMON_HIST_CPU);
    const FmonHistorySeries *mem = fmon_history_series(FMON_HIST_MEM);
    const FmonHistorySeries *swap = fmon_history_series(FMON_HIST_SWAP);
    const FmonHistorySeries *temp = fmon_history_series(FMON_HIST_TEMP);
    const FmonHistorySeries *dtemp = fmon_history_series(FMON_HIST_DISK_TEMP);
    const FmonHistorySeries *rx = fmon_history_series(FMON_HIST_NET_RX);
    const FmonHistorySeries *tx = fmon_history_series(FMON_HIST_NET_TX);
    const FmonHistorySeries *rd = fmon_history_series(FMON_HIST_DISK_RD);
    const FmonHistorySeries *wr = fmon_history_series(FMON_HIST_DISK_WR);

    fputs("Time,CPU(%),Memory(%),Swap(%),Temp(C),Disk_Temp(C),"
          "Net_RX(KB/s),Net_TX(KB/s),Disk_Read(KB/s),Disk_Write(KB/s)\n",
          fp);

    for (uint32_t i = 0; i < n && times != NULL; i++) {
        time_t sec = (time_t)(times[i] / 1000000ULL);
        struct tm tmv;

        if (localtime_r(&sec, &tmv) != NULL) {
            char tb[32];

            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
            fputs(tb, fp);
        }
        fputc(',', fp);
        csv_write_value(fp, cpu ? cpu->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, mem ? mem->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, swap ? swap->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, temp ? temp->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, dtemp ? dtemp->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, rx ? rx->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, tx ? tx->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, rd ? rd->values[i] : NAN, "%.2f");
        fputc(',', fp);
        csv_write_value(fp, wr ? wr->values[i] : NAN, "%.2f");
        fputc('\n', fp);
    }

    return n;
}

static void on_save_finished(GObject *source_object, GAsyncResult *result,
                             gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    (void)user_data;

    GError *error = NULL;
    GFile  *file  = gtk_file_dialog_save_finish(dialog, result, &error);

    if (file == NULL) {
        if (error != NULL) {
            if (!g_error_matches(error, GTK_DIALOG_ERROR,
                                 GTK_DIALOG_ERROR_DISMISSED)) {
                fmon_gui_show_error(g_main_window, _("无法保存文件"), error->message);
            }
            g_error_free(error);
        }
        return;
    }

    /* 关键修复：非本地 GFile（例如 sftp://）没有本地路径，
     * 旧版本直接 fopen(NULL) 会崩溃。 */
    char *path = g_file_get_path(file);
    if (path == NULL) {
        fmon_gui_show_error(g_main_window, _("导出失败"), _("请选择本地文件系统上的位置。"));
        g_object_unref(file);
        return;
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        char *msg = g_strdup_printf(_("无法写入 %s：%s"), path, g_strerror(errno));
        fmon_gui_show_error(g_main_window, _("导出失败"), msg);
        g_free(msg);
        g_free(path);
        g_object_unref(file);
        return;
    }

    if (gui_long_history_active()) {
        guint exported = write_long_history_csv(fp);

        if (ferror(fp) != 0) {
            char *msg = g_strdup_printf(_("写入 %s 时出错：%s"), path,
                                        g_strerror(errno));
            fmon_gui_show_error(g_main_window, _("导出失败"), msg);
            g_free(msg);
        } else {
            char *detail = g_strdup_printf(_("已导出 %u 个长历史采样点到：\n%s"),
                                           exported, path);

            fmon_gui_show_info(g_main_window, _("导出成功"), detail);
            g_free(detail);
        }

        fclose(fp);
        g_free(path);
        g_object_unref(file);
        return;
    }

    fputs("Index,CPU(%),Memory(%),Swap(%),Temp(C),Battery(%),"
          "Net_RX(KB/s),Net_TX(KB/s),Disk_Read(KB/s),Disk_Write(KB/s)\n",
          fp);

    uint32_t valid = gui_state_snapshot()->history_count;
    if (valid > FMON_HISTORY_POINTS)
        valid = FMON_HISTORY_POINTS;

    uint32_t start = (gui_state_snapshot()->history_index + FMON_HISTORY_POINTS - valid) %
                     FMON_HISTORY_POINTS;

    for (uint32_t i = 0; i < valid; i++) {
        uint32_t idx = (start + i) % FMON_HISTORY_POINTS;

        fprintf(fp, "%u,", i);
        csv_write_value(fp, gui_state_snapshot()->cpu_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->mem_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->swap_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->temp_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->battery_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->net_rx_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->net_tx_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->disk_read_history[idx], "%.2f");
        fputc(',', fp);
        csv_write_value(fp, gui_state_snapshot()->disk_write_history[idx], "%.2f");
        fputc('\n', fp);
    }

    if (ferror(fp) != 0) {
        char *msg = g_strdup_printf(_("写入 %s 时出错：%s"), path, g_strerror(errno));
        fmon_gui_show_error(g_main_window, _("导出失败"), msg);
        g_free(msg);
    } else {
        char *detail = g_strdup_printf(_("已导出 %u 个采样点到：\n%s"), valid, path);
        fmon_gui_show_info(g_main_window, "导出成功", detail);
        g_free(detail);
    }

    fclose(fp);
    g_free(path);
    g_object_unref(file);
}

void gui_export_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;

    if (!gui_state_online()) {
        fmon_gui_show_error(g_main_window, _("无法导出"), _("尚未连接到 fmon-daemon，没有可导出的数据。"));
        return;
    }

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "保存监控数据 CSV");
    gtk_file_dialog_set_initial_name(dialog, "fmon_history.csv");
    gtk_file_dialog_set_accept_label(dialog, "保存");

    /* 对话框必须活到异步操作结束，这里自己持有一个引用 */
    g_object_ref(dialog);
    gtk_file_dialog_save(dialog, g_main_window, NULL, on_save_finished, NULL);
    g_object_unref(dialog);
}

/* ===========================================================================
 * 导航
 * ========================================================================= */
/* ===========================================================================
 * 两级导航：分组 → 子页面
 * ========================================================================= */


/* ===========================================================================
 * CSS
 * ========================================================================= */
/* SIGTERM/SIGINT 也走正常关闭流程，保证窗口状态被保存 */
