/*
 * log_filter.c - 日志过滤纯函数（无 GTK）
 */

#include "log_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int fmon_log_syslog_rank(const gchar *lower)
{
    if (lower == NULL)
        return 6;

    /* RFC 3164/5424 的 <N> 优先级：N % 8 就是严重度。 */
    if (lower[0] == '<') {
        int pri = atoi(lower + 1);

        if (pri >= 0 && pri <= 191)
            return pri % 8;
    }

    if (strstr(lower, "emerg") != NULL || strstr(lower, "panic") != NULL)
        return 0;
    if (strstr(lower, "alert") != NULL)
        return 1;
    if (strstr(lower, "critical") != NULL || strstr(lower, "crit") != NULL)
        return 2;
    if (strstr(lower, "error") != NULL || strstr(lower, "fail") != NULL ||
        strstr(lower, "fatal") != NULL || strstr(lower, "denied") != NULL)
        return 3;
    if (strstr(lower, "warn") != NULL || strstr(lower, "deprecated") != NULL)
        return 4;
    if (strstr(lower, "notice") != NULL)
        return 5;
    if (strstr(lower, "debug") != NULL)
        return 7;
    return 6;
}

gboolean fmon_log_level_matches(const gchar *line, LogFilter filter)
{
    if (filter == LOG_FILTER_ALL)
        return TRUE;

    gchar *lower = g_utf8_strdown(line, -1);
    int    rank  = fmon_log_syslog_rank(lower);

    g_free(lower);
    return rank <= (int)filter - 1;
}

gboolean fmon_log_in_range(const gchar *line, gint64 seconds)
{
    if (seconds <= 0)
        return TRUE;

    time_t now = time(NULL);
    int    y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;

    if (sscanf(line, "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &hh, &mm,
               &ss) == 6 ||
        sscanf(line, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss) == 6) {
        struct tm tmv = { 0 };

        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = hh;
        tmv.tm_min = mm;
        tmv.tm_sec = ss;
        tmv.tm_isdst = -1;

        gboolean utc = (strchr(line, 'Z') != NULL &&
                        strchr(line, 'Z') < line + 24);
        time_t t = utc ? timegm(&tmv) : mktime(&tmv);

        return (t == (time_t)-1) || (now - t <= seconds);
    }

    char mon[4] = { 0 };
    int  day = 0;

    if (sscanf(line, "%3s %d %d:%d:%d", mon, &day, &hh, &mm, &ss) != 5)
        return TRUE;

    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int mon_idx = -1;

    for (int i = 0; i < 12; i++)
        if (g_ascii_strcasecmp(mon, months[i]) == 0)
            mon_idx = i;

    if (mon_idx < 0)
        return TRUE;

    struct tm lt;

    localtime_r(&now, &lt);

    struct tm tmv = { 0 };

    tmv.tm_year = lt.tm_year;
    tmv.tm_mon = mon_idx;
    tmv.tm_mday = day;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;
    tmv.tm_isdst = -1;

    time_t t = mktime(&tmv);

    if (t == (time_t)-1)
        return TRUE;

    if (t > now + 86400) {
        tmv.tm_year--;
        t = mktime(&tmv);
    }

    return (t != (time_t)-1) && (now - t <= seconds);
}

gboolean fmon_log_pattern_match(const gchar *line, GPtrArray *patterns,
                                gboolean regex, gboolean case_sensitive)
{
    if (patterns == NULL || patterns->len == 0)
        return TRUE;

    for (guint i = 0; i < patterns->len; i++) {
        const gchar *pat = g_ptr_array_index(patterns, i);
        gboolean     hit = FALSE;

        if (regex) {
            GRegex *re = g_regex_new(pat,
                                     case_sensitive ? 0 : G_REGEX_CASELESS,
                                     0, NULL);

            if (re != NULL) {
                hit = g_regex_match(re, line, 0, NULL);
                g_regex_unref(re);
            }
        } else if (case_sensitive) {
            hit = (strstr(line, pat) != NULL);
        } else {
            gchar *ll = g_utf8_strdown(line, -1);
            gchar *lp = g_utf8_strdown(pat, -1);

            hit = (strstr(ll, lp) != NULL);
            g_free(ll);
            g_free(lp);
        }

        if (!hit)
            return FALSE;
    }

    return TRUE;
}

gchar *fmon_log_dedup_key(const gchar *line)
{
    const gchar *p = strstr(line, "]: ");

    return g_strdup((p != NULL) ? p + 3 : line);
}
