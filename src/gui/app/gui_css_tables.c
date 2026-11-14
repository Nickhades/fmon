/*
 * gui_css_tables.c - 样式片段
 */

#include "gui_css_bits.h"

const char *const gui_css_tables =
/* ---------------- 表格 ---------------- */
        "columnview > listview > row {"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.08);"
        "  min-height: 38px; }"
        "columnview > listview > row:hover {"
        "  background-color: shade(@theme_bg_color, 1.04); }"
        "columnview > listview > row:selected {"
        "  background-color: alpha(@theme_selected_bg_color, 0.22); }"
        "columnview > listview > row > cell { padding: 2px 12px; }"
        "columnview > header { min-height: 36px;"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.12);"
        "  background-color: shade(@theme_bg_color, 1.03); }"
        "columnview > header button { font-size: 12px; opacity: 0.8; font-weight: 600; }"

        /* ---------------- 控件 ---------------- */
        "button { border-radius: 8px; padding: 6px 14px; }"
        "button.suggested-action { border-radius: 8px; }"
        "entry, searchentry { border-radius: 9px; }"
        "dropdown > button { border-radius: 9px; padding: 4px 10px; }"

        "scrollbar slider { min-width: 6px; min-height: 6px;"
        "  border-radius: 3px; background-color: alpha(currentColor, 0.25); }"
        "scrollbar slider:hover { background-color: alpha(currentColor, 0.4); }"

        "notebook > header { background: transparent; }"
        "notebook > header tab { padding: 6px 16px; border-radius: 8px; }"

        /* ---------------- 状态 ---------------- */
        ".banner-warning { background-color: alpha(#e5a50a, 0.16);"
        "  border: 1px solid alpha(#e5a50a, 0.45); border-radius: 10px;"
        "  padding: 8px 12px; }"
        ".banner-warning label { color: #e5a50a; }"

        ".status-dot-running { color: #2ec27e; }"
        ".status-dot-stopped { color: alpha(currentColor, 0.45); }"
        ".danger { color: #e01b24; }"
        /* 表格类页面：与仪表盘卡片一致的外观 */
        ".view-card { background-color: shade(@theme_bg_color, 1.04);"
        "  border: 1px solid alpha(@theme_fg_color, 0.10);"
        "  border-radius: 14px; padding: 12px 14px; }"

        ".view-toolbar { padding-bottom: 6px; }"

        ".alert-row { padding: 8px 10px;"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.08); }"
        ".alert-badge-warning { color: #e5a50a; }"
        ".alert-badge-critical { color: #e01b24; }"
        ".alert-time { opacity: 0.55; font-size: 11px; }"

        ".license-text { font-family: monospace; font-size: 11px; }"
        ".disk-header { padding: 2px 0 6px 0;"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.18); }"
        ".disk-row { padding: 3px 0;"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.07); }"
        ".disk-row:hover { background: alpha(@theme_fg_color, 0.03); }"
        ".license-box { border: 1px solid alpha(@theme_fg_color, 0.12);"
        "  border-radius: 10px; padding: 8px; }"

        ".service-row { padding: 8px 12px;"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.08); }";
