/*
 * gui_css_win11.c - 样式片段
 */

#include "gui_css_bits.h"

const char *const gui_css_win11 =
        "window { background-color: @theme_bg_color; }"

        ".sidebar { background-color: shade(@theme_bg_color, 0.97);"
        "  border-right: 1px solid alpha(@theme_fg_color, 0.08); }"
        ".nav-item-box { background-color: transparent; border: none;"
        "  border-radius: 6px; margin: 2px 8px; padding: 9px 12px; }"
        ".nav-list > row:hover .nav-item-box {"
        "  background-color: alpha(@theme_fg_color, 0.06);"
        "  border-color: transparent; }"
        ".nav-list > row:selected .nav-item-box {"
        "  background-color: alpha(#0078d4, 0.32);"
        "  box-shadow: inset 3px 0 #0078d4;"
        "  border: none; outline: none; }"
        ".nav-list > row:selected .nav-item-box label,"
        ".nav-list > row:selected .nav-item-box image {"
        "  color: @theme_fg_color; }"
        ".nav-list > row:selected .nav-item-box:hover {"
        "  background-color: alpha(#0078d4, 0.38); }"
;
