/*
 * gui_widgets.h - 通用 UI 构件
 */

#ifndef FMON_GUI_WIDGETS_H
#define FMON_GUI_WIDGETS_H

#include <glib.h>
#include <gtk/gtk.h>

/* 详情网格中的一个单元格（key 用 detail-key，值用 detail-value） */
GtkWidget *gui_make_cell(const char *text, const char *css);

/* 带标题的详情卡片；out_grid 返回内部 GtkGrid */
GtkWidget *gui_info_card(const char *title, GtkWidget **out_grid);

/* 面板数据行的统一规则：键定宽 → 值 ELLIPSIZE_END → 列定宽。
 * 行内不再让数据参与宽度谈判（避免刷新时列漂移），字号保持不变。 */
GtkWidget *gui_fixed_row(const char *key, GtkWidget *value, int key_width,
                         int max_chars);

/* 内容签名变化时返回 TRUE，用于避免每秒销毁重建控件树 */
gboolean gui_grid_signature_changed(GtkWidget *grid, const char *sig);

/* 移除网格内所有子控件 */
void gui_grid_clear(GtkWidget *grid);

#endif /* FMON_GUI_WIDGETS_H */
