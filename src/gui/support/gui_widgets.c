/*
 * gui_widgets.c - 通用小控件与详情网格辅助
 *
 * 这里只放与具体页面无关的纯 UI 构件：键值单元格、详情卡片、
 * 网格清空，以及“内容签名不变就不重建”的辅助逻辑。
 */

#include "gui_widgets.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

GtkWidget *gui_info_card(const char *title, GtkWidget **out_grid)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "detail-card");

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "detail-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);

    gtk_box_append(GTK_BOX(card), t);
    gtk_box_append(GTK_BOX(card), grid);

    *out_grid = grid;
    return card;
}

GtkWidget *gui_fixed_row(const char *key, GtkWidget *value, int key_width,
                         int max_chars)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *k = gtk_label_new(key);

    gtk_widget_add_css_class(k, "group-desc");
    gtk_label_set_xalign(GTK_LABEL(k), 0.0f);
    gtk_widget_set_halign(k, GTK_ALIGN_START);

    if (key_width > 0)
        gtk_widget_set_size_request(k, key_width, -1);

    gtk_label_set_xalign(GTK_LABEL(value), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);

    if (max_chars > 0)
        gtk_label_set_max_width_chars(GTK_LABEL(value), max_chars);

    gtk_widget_set_hexpand(value, FALSE);

    gtk_box_append(GTK_BOX(h), k);
    gtk_box_append(GTK_BOX(h), value);
    return h;
}

/* 详情网格带内容签名：值没变就不重建控件树（避免每秒销毁/重建引发 GTK 问题） */
gboolean gui_grid_signature_changed(GtkWidget *grid, const char *sig)
{
    static struct {
        GtkWidget *grid;
        guint      hash;
    } cache[12];

    guint h = g_str_hash(sig);

    for (int i = 0; i < 12; i++) {
        if (cache[i].grid == grid) {
            if (cache[i].hash == h)
                return FALSE;

            cache[i].hash = h;
            return TRUE;
        }
    }

    for (int i = 0; i < 12; i++) {
        if (cache[i].grid == NULL) {
            cache[i].grid = grid;
            cache[i].hash = h;
            return TRUE;
        }
    }

    return TRUE;
}

void gui_grid_clear(GtkWidget *grid)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(grid)) != NULL)
        gtk_grid_remove(GTK_GRID(grid), child);
}

GtkWidget *gui_make_cell(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text);

    if (css != NULL)
        gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    /* 不启用文本选择：详情卡每秒会重建，选中的标签一旦被销毁会让 GTK
     * 焦点/选区块指向已释放对象（复制文本时 SIGBUS）。数据可用「导出数据」获取。 */
    gtk_label_set_selectable(GTK_LABEL(l), FALSE);
    return l;
}

