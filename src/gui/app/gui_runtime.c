/*
 * gui_runtime.c - 运行时状态：共享内存快照、连接横幅、顶部信息、告警指示
 */

#include "gui_runtime.h"
#include "gui_runtime_internal.h"

#include "gui_state.h"
#include "gui_util.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

static const AlertSettings *s_alerts = NULL;

void gui_runtime_alert_indicator(void)
{
    static AlertSettings settings;
    static gboolean      loaded = FALSE;

    if (!loaded) {
        alerts_load(&settings);
        loaded = TRUE;
    }

    gchar *summary = alerts_check(&g_snap, &settings);

    g_free(g_alert_summary);
    g_alert_summary = summary;

    if (g_alert_label == NULL)
        return;

    if (summary != NULL) {
        gtk_label_set_text(GTK_LABEL(g_alert_label), summary);
        gtk_widget_set_visible(g_alert_label, TRUE);
    } else {
        gtk_widget_set_visible(g_alert_label, FALSE);
    }
}

void gui_runtime_banner(void)
{
    gchar *text = NULL;

    if (g_live == NULL) {
        text = g_strdup_printf(_("未连接到 fmon-daemon：%s"),
                               (g_link_error != NULL) ? g_link_error : "未知原因");
    } else if (g_snap.status == (int32_t)FMON_STATUS_STOPPED) {
        text = g_strdup( _("fmon-daemon 已停止运行，下面显示的是最后一次采集的数据。") );
    } else if (!g_online) {
        if (g_snap.last_update_us != 0) {
            uint64_t now = gui_monotonic_us();
            uint64_t age_ms = (now > g_snap.last_update_us)
                                  ? (now - g_snap.last_update_us) / 1000u
                                  : 0u;
            text = g_strdup_printf(
                "数据已 %" G_GUINT64_FORMAT " 秒未更新，守护进程可能被挂起。",
                age_ms / 1000u);
        } else {
            text = g_strdup( _("正在等待守护进程发布第一份数据…") );
        }
    }

    if (text != NULL) {
        gtk_label_set_text(GTK_LABEL(g_banner_label), text);
        gtk_widget_set_visible(g_banner, TRUE);
        g_free(text);
    } else {
        gtk_widget_set_visible(g_banner, FALSE);
    }
}

void gui_runtime_header(const char *title, const char *page_name)
{
    (void)title;
    (void)page_name;

    if (!g_ever_linked) {
        gtk_label_set_text(GTK_LABEL(g_header_sub), _("等待守护进程…请确认 fmon-daemon 已启动") );
        return;
    }

    char *uptime = fmon_gui_format_uptime(g_snap.uptime_sec);
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%H:%M:%S");

    char *sub = g_strdup_printf(_("%s · %s · 运行 %s · %u 个进程 · 更新 %s"),
                                (g_snap.hostname[0] != '\0') ? g_snap.hostname
                                                             : "unknown",
                                (g_snap.sys_version[0] != '\0')
                                    ? g_snap.sys_version
                                    : "FreeBSD",
                                uptime, g_snap.proc_total, stamp);
    gtk_label_set_text(GTK_LABEL(g_header_sub), sub);
    g_free(sub);
    g_free(stamp);
    g_date_time_unref(now);
    g_free(uptime);
}

/* ===========================================================================
 * 卡片
 * ========================================================================= */

void gui_runtime_bind(GtkWidget *header_sub, GtkWidget *banner,
                      GtkWidget *banner_label, GtkWidget *alert_label,
                      const AlertSettings *settings)
{
    g_header_sub   = header_sub;
    g_banner       = banner;
    g_banner_label = banner_label;
    g_alert_label  = alert_label;
    s_alerts       = settings;
}
