/*
 * csv_util.c - CSV 文本字段转义
 */

#include "csv_util.h"

#include <string.h>

static gboolean csv_formula_prefix(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return FALSE;

    switch (text[0]) {
    case '=':
    case '+':
    case '-':
    case '@':
    case '\t':
    case '\r':
        return TRUE;
    default:
        return FALSE;
    }
}

char *fmon_csv_escape(const char *text)
{
    const char *s = (text != NULL) ? text : "";
    GString    *out = g_string_new("\"");

    if (csv_formula_prefix(s))
        g_string_append_c(out, '\'');

    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '"')
            g_string_append(out, "\"\"");
        else
            g_string_append_c(out, *p);
    }

    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}
