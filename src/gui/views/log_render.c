/*
 * log_render.c - 日志过滤、PID 标记与上下文提取
 */

#include "log_render.h"

#include "log_filter.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <string.h>

static LogFilter current_filter(LogPage *page)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->level_dd));

    if (sel == GTK_INVALID_LIST_POSITION)
        return LOG_FILTER_ALL;
    return (LogFilter)sel;
}

GString *log_render_filter(LogPage *page, const gchar *text)
{
    LogFilter    filter = current_filter(page);
    const gchar *needle = gtk_editable_get_text(GTK_EDITABLE(page->search));
    const gchar *excl   = gtk_editable_get_text(GTK_EDITABLE(page->exclude));
    gboolean     regex  = gtk_toggle_button_get_active(
                              GTK_TOGGLE_BUTTON(page->regex_btn));
    gboolean     dedup = gtk_toggle_button_get_active(
                             GTK_TOGGLE_BUTTON(page->dedup_btn));

    GPtrArray *inc = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *exc = g_ptr_array_new_with_free_func(g_free);
    gchar    **tok = g_strsplit((needle != NULL) ? needle : "", " ", -1);

    for (int i = 0; tok[i] != NULL; i++) {
        if (tok[i][0] == '\0')
            continue;
        if (tok[i][0] == '-' && tok[i][1] != '\0')
            g_ptr_array_add(exc, g_strdup(tok[i] + 1));
        else
            g_ptr_array_add(inc, g_strdup(tok[i]));
    }
    g_strfreev(tok);

    if (excl != NULL && excl[0] != '\0') {
        gchar **et = g_strsplit(excl, " ", -1);

        for (int i = 0; et[i] != NULL; i++)
            if (et[i][0] != '\0')
                g_ptr_array_add(exc, g_strdup(et[i]));
        g_strfreev(et);
    }

    GString *out   = g_string_new(NULL);
    gchar  **lines = g_strsplit(text, "\n", -1);
    guint    count = 0;
    guint    total = g_strv_length(lines);
    guint    start = (total > LOG_MAX_LINES) ? total - LOG_MAX_LINES : 0;

    gchar *pending = NULL;
    gchar *pkey = NULL;
    guint  dup = 0;

    for (guint i = start; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;
        if (!fmon_log_level_matches(lines[i], filter))
            continue;

        guint rsel = gtk_drop_down_get_selected(
            GTK_DROP_DOWN(page->range_dd));
        static const gint64 ranges[] = { 0, 3600, 6 * 3600, 24 * 3600 };

        if (rsel < G_N_ELEMENTS(ranges) &&
            !fmon_log_in_range(lines[i], ranges[rsel]))
            continue;

        if (!fmon_log_pattern_match(lines[i], inc, regex, FALSE))
            continue;
        if (exc->len > 0 && fmon_log_pattern_match(lines[i], exc, regex, FALSE))
            continue;

        if (dedup) {
            gchar *key = fmon_log_dedup_key(lines[i]);

            if (pkey != NULL && strcmp(pkey, key) == 0) {
                dup++;
                g_free(key);
                continue;
            }

            if (pending != NULL) {
                g_string_append(out, pending);
                if (dup > 1)
                    g_string_append_printf(out, _("（重复 %u 次）"), dup);
                g_string_append_c(out, '\n');
                count++;
            }

            g_free(pending);
            g_free(pkey);
            pending = g_strdup(lines[i]);
            pkey = key;
            dup = 1;
        } else {
            g_string_append(out, lines[i]);
            g_string_append_c(out, '\n');
            count++;
        }
    }

    if (pending != NULL) {
        g_string_append(out, pending);
        if (dup > 1)
            g_string_append_printf(out, _("（重复 %u 次）"), dup);
        g_string_append_c(out, '\n');
        count++;
    }

    g_free(pending);
    g_free(pkey);
    g_ptr_array_free(inc, TRUE);
    g_ptr_array_free(exc, TRUE);
    g_strfreev(lines);

    if (count == 0)
        g_string_append(out, _("（没有匹配的日志行）\n"));

    return out;
}

/* ===========================================================================
 * 异步读取
 * ========================================================================= */
gboolean log_render_at_bottom(LogPage *page)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(page->scroll));

    if (adj == NULL)
        return TRUE;

    return (gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) -
            gtk_adjustment_get_value(adj)) < 32.0;
}

void log_render_apply_pid_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *tab = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(tab, "pid-link");

    if (tag == NULL)
        tag = gtk_text_buffer_create_tag(buffer, "pid-link", "foreground",
                                         "#4aa3ff", "underline",
                                         PANGO_UNDERLINE_SINGLE, NULL);

    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    GRegex *re = g_regex_new("\\[[0-9]{2,7}\\]|pid [0-9]{2,7}",
                             G_REGEX_CASELESS, 0, NULL);

    if (re != NULL) {
        GMatchInfo *mi = NULL;

        g_regex_match(re, text, 0, &mi);

        while (g_match_info_matches(mi)) {
            int s = 0, e = 0;

            g_match_info_fetch_pos(mi, 0, &s, &e);

            GtkTextIter a, b;

            gtk_text_buffer_get_iter_at_offset(buffer, &a, s);
            gtk_text_buffer_get_iter_at_offset(buffer, &b, e);
            gtk_text_buffer_apply_tag(buffer, tag, &a, &b);
            g_match_info_next(mi, NULL);
        }

        g_match_info_free(mi);
        g_regex_unref(re);
    }

    g_free(text);
}

gchar *log_render_raw_context(LogPage *page, const gchar *needle)
{
    if (page->raw == NULL || needle == NULL || needle[0] == '\0')
        return NULL;

    const gchar *base = page->raw->str;
    const gchar *p    = g_strstr_len(base, (gssize)page->raw->len, needle);

    if (p == NULL)
        return NULL;

    const gchar *s = p;
    const gchar *e = p;

    for (int i = 0; i < 5; i++) {
        while (s > base && s[-1] != '\n')
            s--;
        if (s > base)
            s--;
    }

    for (int i = 0; i < 5; i++) {
        while (*e != '\0' && *e != '\n')
            e++;
        if (*e == '\n')
            e++;
    }

    return g_strndup(s, (gsize)(e - s));
}

void log_render_refilter(LogPage *page)
{
    if (page->raw == NULL)
        return;

    gboolean was_at_bottom = log_render_at_bottom(page);
    GString *filtered = log_render_filter(page, page->raw->str);
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *current = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (g_strcmp0(current, filtered->str) != 0) {
        gtk_text_buffer_set_text(buffer, filtered->str, -1);
        log_render_apply_pid_tags(buffer);

        if (page->follow && was_at_bottom) {
            GtkTextIter e;

            gtk_text_buffer_get_end_iter(buffer, &e);
            gtk_text_buffer_place_cursor(buffer, &e);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(page->text_view), &e,
                                         0.0, FALSE, 0, 0);
        }
    }

    g_free(current);
    g_string_free(filtered, TRUE);
}
