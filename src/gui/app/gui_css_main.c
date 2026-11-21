/*
 * gui_css_main.c - 样式片段
 */

#include "gui_css_bits.h"

const char *const gui_css_main =
"* { font-weight: bold; font-family: \"Noto Sans\", \"Noto Sans CJK SC\", \"DejaVu Sans\", Sans; }"

        "window { background-color: @theme_bg_color; }"

        /* ---------------- 侧边栏 ---------------- */
        ".sidebar { background-color: shade(@theme_bg_color, 0.94);"
        "  border-right: 1px solid alpha(@theme_fg_color, 0.10);"
        "  padding: 14px 10px; }"

        ".nav-list { background: transparent; outline: none; }"
        ".nav-list > row { background: transparent; padding: 0; margin: 0;"
        "  outline: none; border: none; }"

        ".nav-header { font-size: 11px; opacity: 0.5; }"
        ".nav-item-box { background-color: alpha(@theme_fg_color, 0.045);"
        "  border-radius: 0; margin: 6px 6px; padding: 12px 14px;"
        "  border: 1px solid transparent; }"

        ".nav-list > row:hover .nav-item-box {"
        "  background-color: alpha(@theme_fg_color, 0.09);"
        "  border-color: alpha(@theme_fg_color, 0.16); }"

        /* 选中项：中性高亮（比背景略亮），文字始终用主题前景色，
         * 浅色/深色主题下都能看清。 */
        ".nav-list > row:selected,"
        ".nav-list > row:selected:hover,"
        ".nav-list > row:selected:focus,"
        ".nav-list > row:selected:focus-visible {"
        "  background-color: transparent;"
        "  background-image: none;"
        "  border: none;"
        "  outline: none;"
        "  box-shadow: none; }"

        ".nav-list > row:selected .nav-item-box {"
        "  background-color: alpha(@theme_fg_color, 0.14);"
        "  border-color: alpha(@theme_fg_color, 0.24);"
        "  box-shadow: none;"
        "  outline: none; }"

        ".nav-list > row:selected .nav-item-box label,"
        ".nav-list > row:selected .nav-item-box image {"
        "  color: @theme_fg_color; }"

        ".nav-list > row:selected .nav-item-box label {"
        "  font-weight: 800; }"

        ".nav-list > row:selected .nav-item-box:hover {"
        "  background-color: alpha(@theme_fg_color, 0.18); }"

        /* ---------------- 内容区 ---------------- */
        ".content-area { padding: 16px 24px 16px 24px; }"
        ".header { font-size: 24px; letter-spacing: 0.3px; }"
        ".subheader { font-size: 13px; opacity: 0.72; }"

        ".card { padding: 16px; border-radius: 0;"
        "  background-color: shade(@theme_bg_color, 1.06);"
        "  border: 1px solid alpha(@theme_fg_color, 0.11);"
        "  border-left-width: 3px;"
        "  box-shadow: none;"
        "  transition: background-color 120ms ease, border-color 120ms ease; }"

        ".card:hover { background-color: shade(@theme_bg_color, 1.10);"
        "  border-color: alpha(@theme_fg_color, 0.17); }"

        ".accent-green  { border-left-color: #2ec27e; }"
        ".accent-blue   { border-left-color: #3584e4; }"
        ".accent-orange { border-left-color: #ff7800; }"
        ".accent-cyan   { border-left-color: #26c6da; }"
        ".accent-red    { border-left-color: #e01b24; }"
        ".accent-amber  { border-left-color: #e5a50a; }"
        ".accent-purple { border-left-color: #c061cb; }"

        ".big-number { font-size: 27px; font-family: monospace; font-weight: bold; }"
        ".title { font-size: 13px; opacity: 0.9; font-weight: 600; }"
        ".small-text { font-size: 12px; opacity: 0.68; }"
        ".hw-model { font-size: 12px; opacity: 0.66; }"
        ".dim-label { opacity: 0.65; }"

        /* ---------------- 主题分段开关 ---------------- */
        ".theme-switch { background: transparent; box-shadow: none; }"
        ".theme-switch button, .theme-switch button:hover,"
        ".theme-switch button:checked, .theme-switch button:active,"
        ".theme-switch button:focus, .theme-switch button:focus-visible {"
        "  background: transparent; background-image: none; box-shadow: none;"
        "  border: none; outline: none; text-shadow: none;"
        "  -gtk-icon-shadow: none; border-radius: 0;"
        "  min-width: 0; min-height: 0; padding: 0; }"
        ".theme-switch .theme-seg:hover {"
        "  background-color: alpha(@theme_fg_color, 0.06); }"
        /* 图标由 cairo 直接绘制，按钮只保留点击区域 */
        ".theme-switch .theme-seg image { color: transparent; }";
