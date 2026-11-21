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

    /* 终端风格覆盖：等宽字体、直角、1px 边框、无阴影/渐变。 */
    static const char *const terminal_css =
        "* {"
        "  font-family: \"JetBrains Mono\", \"DejaVu Sans Mono\","
        "               \"Noto Sans Mono CJK SC\", monospace;"
        "  border-radius: 0; box-shadow: none;"
        "  -gtk-icon-shadow: none; text-shadow: none;"
        "  background-image: none; }"
        ".card, .page-card, .view-card, .detail-card, .metric-detail,"
        ".alert-row, .group-tile {"
        "  border-radius: 0; box-shadow: none;"
        "  border: 1px solid alpha(@theme_fg_color, 0.45);"
        "  border-left-width: 1px; }"
        ".card { padding: 16px; }"
        "button, button.suggested-action, .suggested-action, entry,"
        "searchentry, dropdown, dropdown > button, dropdown button,"
        "notebook > header tab, .nav-item-box, .theme-switch,"
        ".theme-switch button, .metric-detail, .detail-card, .group-tile,"
        ".card, .page-card, .view-card {"
        "  border-radius: 0; box-shadow: none;"
        "  background-image: none; }"
        ".metric-detail, .detail-card, .group-tile {"
        "  border: 1px solid alpha(@theme_fg_color, 0.45); }"
        ".header-action { padding: 4px 8px; min-height: 24px; }"
        ".quick-actions { margin-top: -2px; margin-bottom: 2px; }"
        "checkbutton > check { min-width: 16px; min-height: 16px;"
        "  border: 1px solid alpha(@theme_fg_color, 0.55);"
        "  background-color: @theme_base_color;"
        "  -gtk-icon-source: none; }"
        "checkbutton > check:checked {"
        "  background-color: @theme_selected_bg_color;"
        "  color: @theme_selected_fg_color;"
        "  border-color: @theme_selected_bg_color;"
        "  -gtk-icon-source: -gtk-icontheme(\"object-select-symbolic\"); }"
        ".lang-switch { padding: 8px; border-top: 1px solid alpha(@theme_fg_color, 0.12); }"
        ".lang-button { padding: 3px 8px; min-height: 24px; }"
        ".lang-button:checked { background: @theme_selected_bg_color;"
        "  color: @theme_selected_fg_color; }"
        ".page-tabs { margin: 0 0 2px 0; }"
        ".page-tab-row { padding: 0; }"
        ".page-tab { padding: 5px 12px; margin-right: 4px; min-height: 22px;"
        "  border: 1px solid alpha(@theme_fg_color, 0.28);"
        "  background: transparent; }"
        ".page-tab:hover { background: alpha(@theme_fg_color, 0.08); }"
        ".page-tab:checked { background: @theme_selected_bg_color;"
        "  color: @theme_selected_fg_color;"
        "  border-color: @theme_selected_bg_color; }"
        "window.fmon-dark { background-color: #232323; color: #e8e8e8; }"
        "window.fmon-dark .sidebar { background-color: #1d1d1d; }"
        "window.fmon-dark .content-area { background-color: #232323; }"
        "window.fmon-dark .card,"
        "window.fmon-dark .page-card,"
        "window.fmon-dark .view-card,"
        "window.fmon-dark .detail-card,"
        "window.fmon-dark .metric-detail,"
        "window.fmon-dark .group-tile {"
        "  background-color: #2a2a2a;"
        "  border-color: alpha(#ffffff, 0.30); }";

    gchar *css = g_strconcat(gui_css_main, gui_css_tables, gui_css_win11,
                             gui_css_win11b, terminal_css, NULL);

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
