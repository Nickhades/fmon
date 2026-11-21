/*
 * i18n_ui.c - UI 文本翻译实现
 */

#define FMON_I18N_IMPL 1

#pragma GCC diagnostic ignored "-Wformat-nonliteral"

#include "i18n_ui.h"
#include "i18n_phrases.h"

#undef gtk_label_new
#undef gtk_label_set_text
#undef gtk_label_set_markup
#undef gtk_button_new_with_label
#undef gtk_check_button_new_with_label
#undef gtk_toggle_button_new_with_label
#undef gtk_button_set_label
#undef gtk_widget_set_tooltip_text
#undef g_snprintf
#undef g_strdup_printf
#undef g_string_append_printf
#undef cairo_show_text
#undef pango_layout_set_text
#undef gtk_drop_down_new_from_strings
#undef gtk_file_dialog_set_title
#undef gtk_file_dialog_set_accept_label
#undef gtk_alert_dialog_set_detail
#undef gtk_alert_dialog_set_buttons

#include <glib/gi18n.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static GString *s_phrase_buf;
static GPtrArray *s_phrase_tr;
static int s_phrase_enabled = -1;

static gboolean phrase_enabled(void)
{
    if (s_phrase_enabled < 0) {
        const char *probe = dgettext(GETTEXT_PACKAGE, "设备正常");

        s_phrase_enabled = (probe != NULL && strcmp(probe, "设备正常") != 0);
    }
    return s_phrase_enabled != 0;
}

static void phrase_init(void)
{
    if (s_phrase_tr != NULL)
        return;

    s_phrase_tr = g_ptr_array_new();

    for (int i = 0; fmon_i18n_phrases[i] != NULL; i++) {
        const char *tr = dgettext(GETTEXT_PACKAGE, fmon_i18n_phrases[i]);

        if (tr != NULL && strcmp(tr, fmon_i18n_phrases[i]) != 0)
            g_ptr_array_add(s_phrase_tr, (gpointer)tr);
        else
            g_ptr_array_add(s_phrase_tr, NULL);
    }
}

static gboolean has_cjk(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0';) {
        if (p[0] >= 0xe4 && p[0] <= 0xe9)
            return TRUE;
        p++;
    }
    return FALSE;
}

/* 复合文本（例如“设备正常 · 使用率 12%”）没有整串翻译时，
 * 用最长匹配的短语替换，避免未包 _() 的拼接文本在中英之间混排。 */
static const struct {
    const char *zh;
    const char *en;
} phrase_extra[] = {
    { "系统平均负载", "System average load" },
    { "平均负载", "Average load" },
    { "总使用率", "Total usage" },
    { "负载", "Load" },
    { "裸机", "Bare metal" },
    { "客户机", "Guest" },
    { "运行中", "Running" },
    { "数未知", "unknown count" },
    { "已启用", "enabled" },
    { "未启用", "disabled" },
    { "阈值", "threshold" },
    { "系统消息", "System messages" },
    { "全部级别", "All levels" },
    { "全部时间", "All time" },
};

static const char *phrase_extra_match(const char *p, gsize *len)
{
    for (guint i = 0; i < G_N_ELEMENTS(phrase_extra); i++) {
        gsize n = strlen(phrase_extra[i].zh);

        if (g_str_has_prefix(p, phrase_extra[i].zh)) {
            *len = n;
            return phrase_extra[i].en;
        }
    }
    return NULL;
}

static const char *phrase_translate(const char *text)
{
    if (text == NULL || *text == '\0' || !has_cjk(text) || !phrase_enabled())
        return text;

    phrase_init();

    if (s_phrase_buf == NULL)
        s_phrase_buf = g_string_new(NULL);
    g_string_truncate(s_phrase_buf, 0);

    const char *p = text;

    while (*p != '\0') {
        int best = -1;
        gsize best_len = 0;
        gsize extra_len = 0;
        const char *extra = phrase_extra_match(p, &extra_len);

        for (guint i = 0; i < s_phrase_tr->len; i++) {
            const char *src = fmon_i18n_phrases[i];
            gsize len = strlen(src);

            if (len > best_len && g_str_has_prefix(p, src) &&
                g_ptr_array_index(s_phrase_tr, i) != NULL) {
                best = (int)i;
                best_len = len;
            }
        }

        if (extra != NULL && extra_len > best_len) {
            g_string_append(s_phrase_buf, extra);
            p += extra_len;
        } else if (best >= 0) {
            g_string_append(s_phrase_buf,
                            (const char *)g_ptr_array_index(s_phrase_tr, best));
            p += best_len;
        } else {
            g_string_append_c(s_phrase_buf, *p);
            p++;
        }
    }

    return s_phrase_buf->str;
}

const char *fmon_i18n_tr(const char *text)
{
    if (text == NULL || *text == '\0')
        return text;

    const char *exact = dgettext(GETTEXT_PACKAGE, text);

    if (exact != NULL && strcmp(exact, text) != 0)
        return exact;

    return phrase_translate(text);
}

GtkWidget *fmon_i18n_label_new(const char *text)
{
    return gtk_label_new(fmon_i18n_tr(text));
}

void fmon_i18n_label_set_text(GtkLabel *label, const char *text)
{
    gtk_label_set_text(label, fmon_i18n_tr(text));
}

void fmon_i18n_label_set_markup(GtkLabel *label, const char *text)
{
    gtk_label_set_markup(label, fmon_i18n_tr(text));
}

GtkWidget *fmon_i18n_button_new_with_label(const char *text)
{
    return gtk_button_new_with_label(fmon_i18n_tr(text));
}

GtkWidget *fmon_i18n_check_button_new_with_label(const char *text)
{
    return gtk_check_button_new_with_label(fmon_i18n_tr(text));
}

GtkWidget *fmon_i18n_toggle_button_new_with_label(const char *text)
{
    return gtk_toggle_button_new_with_label(fmon_i18n_tr(text));
}

void fmon_i18n_button_set_label(GtkButton *button, const char *text)
{
    gtk_button_set_label(button, fmon_i18n_tr(text));
}

void fmon_i18n_tooltip_text(GtkWidget *widget, const char *text)
{
    gtk_widget_set_tooltip_text(widget, fmon_i18n_tr(text));
}

int fmon_i18n_snprintf(char *buf, size_t n, const char *format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = vsnprintf(buf, n, fmon_i18n_tr(format), ap);
    va_end(ap);
    return ret;
}

char *fmon_i18n_strdup_printf(const char *format, ...)
{
    va_list ap;
    char *ret;

    va_start(ap, format);
    ret = g_strdup_vprintf(fmon_i18n_tr(format), ap);
    va_end(ap);
    return ret;
}

void fmon_i18n_string_append_printf(GString *string, const char *format, ...)
{
    va_list ap;
    char *text;

    va_start(ap, format);
    text = g_strdup_vprintf(fmon_i18n_tr(format), ap);
    va_end(ap);

    g_string_append(string, text);
    g_free(text);
}

void fmon_i18n_cairo_show_text(cairo_t *cr, const char *text)
{
    cairo_show_text(cr, fmon_i18n_tr(text));
}

void fmon_i18n_pango_set_text(PangoLayout *layout, const char *text, int len)
{
    pango_layout_set_text(layout, fmon_i18n_tr(text), len);
}

GtkWidget *fmon_i18n_drop_down_new(const char *const *strings)
{
    const char **translated = g_new0(const char *, g_strv_length((char **)strings) + 1);
    int n = 0;

    for (int i = 0; strings[i] != NULL; i++)
        translated[n++] = g_strdup(fmon_i18n_tr(strings[i]));
    translated[n] = NULL;

    /* 保留副本：复合翻译使用静态缓冲区，不能把所有条目指向同一块内存。 */
    GtkWidget *widget = gtk_drop_down_new_from_strings(translated);
    g_free(translated);
    return widget;
}

void fmon_i18n_file_dialog_title(GtkFileDialog *dialog, const char *title)
{
    gtk_file_dialog_set_title(dialog, fmon_i18n_tr(title));
}

void fmon_i18n_file_accept_label(GtkFileDialog *dialog, const char *text)
{
    gtk_file_dialog_set_accept_label(dialog, fmon_i18n_tr(text));
}

void fmon_i18n_alert_detail(GtkAlertDialog *dialog, const char *text)
{
    gtk_alert_dialog_set_detail(dialog, fmon_i18n_tr(text));
}

void fmon_i18n_alert_buttons(GtkAlertDialog *dialog, const char *const *labels)
{
    const char **translated = g_new0(const char *, g_strv_length((char **)labels) + 1);
    int n = 0;

    for (int i = 0; labels[i] != NULL; i++)
        translated[n++] = g_strdup(fmon_i18n_tr(labels[i]));
    translated[n] = NULL;

    gtk_alert_dialog_set_buttons(dialog, translated);
    g_free(translated);
}
