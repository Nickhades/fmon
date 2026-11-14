/*
 * gui_css.c - 全局样式表（GTK CSS）
 *
 * 从 main.c 拆出：样式与业务逻辑分离，便于单独调整视觉而不碰窗口代码。
 */

#include "gui_css.h"
#include "gui_css_bits.h"

#include <gtk/gtk.h>

void gui_css_load(void)
{

    /* ---------------- Windows 11 风格覆盖 ---------------- */

    gchar *css = g_strconcat(gui_css_main, gui_css_tables, gui_css_win11,
                             gui_css_win11b, NULL);

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    g_free(css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* --------------------------------------------------------------------------
 * 详情卡片：标题 + 可重建的网格（用于网卡 / 磁盘明细）
 * -------------------------------------------------------------------------- */
