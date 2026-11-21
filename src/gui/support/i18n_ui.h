/*
 * i18n_ui.h - 统一 UI 文本翻译入口
 *
 * 通过 -include 覆盖 GTK 文本构造函数、格式化和 Cairo 文本输出，
 * 使没有逐个包 _() 的历史字符串也能在同一次重启后统一翻译。
 * 仅用于 GUI 可执行文件；实现文件定义 FMON_I18N_IMPL 避免宏递归。
 */

#ifndef FMON_I18N_UI_H
#define FMON_I18N_UI_H

#include <cairo.h>
#include <gtk/gtk.h>

const char *fmon_i18n_tr(const char *text);

#ifndef FMON_I18N_IMPL
#define gtk_label_new(text) fmon_i18n_label_new(text)
#define gtk_label_set_text(label, text) fmon_i18n_label_set_text((label), (text))
#define gtk_label_set_markup(label, text) fmon_i18n_label_set_markup((label), (text))
#define gtk_button_new_with_label(text) fmon_i18n_button_new_with_label(text)
#define gtk_check_button_new_with_label(text) fmon_i18n_check_button_new_with_label(text)
#define gtk_toggle_button_new_with_label(text) fmon_i18n_toggle_button_new_with_label(text)
#define gtk_button_set_label(button, text) fmon_i18n_button_set_label((button), (text))
#define gtk_widget_set_tooltip_text(widget, text) fmon_i18n_tooltip_text((widget), (text))
#define g_snprintf(...) fmon_i18n_snprintf(__VA_ARGS__)
#define g_strdup_printf(...) fmon_i18n_strdup_printf(__VA_ARGS__)
#define g_string_append_printf(...) fmon_i18n_string_append_printf(__VA_ARGS__)
#define cairo_show_text(cr, text) fmon_i18n_cairo_show_text((cr), (text))
#define pango_layout_set_text(layout, text, len) fmon_i18n_pango_set_text((layout), (text), (len))
#define gtk_drop_down_new_from_strings(strings) fmon_i18n_drop_down_new((strings))
#define gtk_file_dialog_set_title(dialog, title) fmon_i18n_file_dialog_title((dialog), (title))
#define gtk_file_dialog_set_accept_label(dialog, text) fmon_i18n_file_accept_label((dialog), (text))
#define gtk_alert_dialog_set_detail(dialog, text) fmon_i18n_alert_detail((dialog), (text))
#endif

GtkWidget *fmon_i18n_label_new(const char *text);
void fmon_i18n_label_set_text(GtkLabel *label, const char *text);
void fmon_i18n_label_set_markup(GtkLabel *label, const char *text);
GtkWidget *fmon_i18n_button_new_with_label(const char *text);
GtkWidget *fmon_i18n_check_button_new_with_label(const char *text);
GtkWidget *fmon_i18n_toggle_button_new_with_label(const char *text);
void fmon_i18n_button_set_label(GtkButton *button, const char *text);
void fmon_i18n_tooltip_text(GtkWidget *widget, const char *text);
int fmon_i18n_snprintf(char *buf, size_t n, const char *format, ...);
char *fmon_i18n_strdup_printf(const char *format, ...);
void fmon_i18n_string_append_printf(GString *string, const char *format, ...);
void fmon_i18n_cairo_show_text(cairo_t *cr, const char *text);
void fmon_i18n_pango_set_text(PangoLayout *layout, const char *text, int len);
GtkWidget *fmon_i18n_drop_down_new(const char *const *strings);
void fmon_i18n_file_dialog_title(GtkFileDialog *dialog, const char *title);
void fmon_i18n_file_accept_label(GtkFileDialog *dialog, const char *text);
void fmon_i18n_alert_detail(GtkAlertDialog *dialog, const char *text);
void fmon_i18n_alert_buttons(GtkAlertDialog *dialog, const char *const *labels);

#endif
