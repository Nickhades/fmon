/*
 * log_view.c - 系统日志查看器
 *
 * 旧版本的问题：
 *   1. 每 3 秒在 GTK 主线程里 popen("tail -n 100 ...")，读取时界面会卡。
 *   2. 定时器永远不会停止：窗口关闭后回调仍然执行，而 g_log_text_view
 *      已经被销毁 —— 这是一个典型的 use-after-free（GTK4 的普通控件
 *      没有 destroy 信号，必须在控件销毁前主动移除 source）。
 *   3. 只能看 /var/log/messages，不能搜索、不能暂停、不能过滤。
 *
 * 现在：
 *   - tail 在后台线程执行（GTask），主线程不阻塞；
 *   - 定时器通过 g_object_set_data_full 绑定控件生命周期，控件销毁即移除；
 *     异步回调期间对根控件加引用，回调返回后再释放；
 *   - 增加文件选择、搜索、级别过滤、暂停/继续、自动滚动。
 */

#include <glib/gi18n.h>

#include "log_view.h"

#include "gui_util.h"
#include "gui_window.h"
#include "process_view.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_MAX_LINES 2000

typedef enum {
    LOG_FILTER_ALL = 0,
    LOG_FILTER_EMERG,
    LOG_FILTER_ALERT,
    LOG_FILTER_CRIT,
    LOG_FILTER_ERR,
    LOG_FILTER_WARN,
    LOG_FILTER_NOTICE,
    LOG_FILTER_INFO,
    LOG_FILTER_DEBUG
} LogFilter;

typedef struct {
    gchar *label;
    gchar *path;   /* 文件源；与 cmd 二选一 */
    gchar *cmd;    /* 命令源（dmesg / zpool events） */
} LogSource;

typedef struct {
    GtkWidget *root;        /* 根容器，异步期间持有引用 */
    GtkWidget *text_view;
    GtkWidget *scroll;
    GtkWidget *search;
    GtkWidget *exclude;
    GtkWidget *regex_btn;
    GtkWidget *dedup_btn;
    GtkWidget *follow_btn;
    GtkWidget *path_label;
    GtkWidget *level_dd;
    GtkWidget *preset_dd;
    GtkWidget *range_dd;

    GPtrArray *sources;     /* LogSource* */
    gchar    *path;
    gchar    *cmd;
    GString  *raw;          /* 累积的原始日志（只含完整行） */
    GString  *pending;      /* 尚未收完的半行 */
    goffset   offset;       /* 文件已读偏移 */
    gboolean  initialized;  /* 是否已完成首次加载 */
    time_t    file_mtime;   /* 用于缓存失效判断 */
    gboolean  append;       /* 本次是增量读取 */
    gboolean  follow;
    gboolean  busy;
    guint     generation;   /* 日志源切换代数，丢弃过期异步结果 */
    guint     req_gen;
    GFileMonitor *monitor;  /* 文件变化监听，接近实时 */
    GHashTable  *ctx_cache; /* 同一行重复双击时避免再次扫描大文件 */
    guint     timer;
} LogPage;

static void log_page_free(gpointer data);
static void start_tail(LogPage *page);

/* ===========================================================================
 * 过滤
 * ========================================================================= */
static LogFilter current_filter(LogPage *page)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->level_dd));

    if (sel == GTK_INVALID_LIST_POSITION)
        return LOG_FILTER_ALL;
    return (LogFilter)sel;
}

/* syslog 严重度：0 emerg … 7 debug；无法识别按 info(6) 处理 */
static int syslog_rank(const gchar *lower)
{
    /* RFC 3164/5424 的 <N> 优先级：N % 8 就是严重度 */
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

static gboolean line_matches_level(const gchar *line, LogFilter filter)
{
    if (filter == LOG_FILTER_ALL)
        return TRUE;

    gchar *lower = g_utf8_strdown(line, -1);
    int    rank  = syslog_rank(lower);

    g_free(lower);
    return rank <= (int)filter - 1;
}

/* ===========================================================================
 * 文本处理
 * ========================================================================= */
static gboolean line_in_range(const gchar *line, gint64 seconds)
{
    if (seconds <= 0)
        return TRUE;

    time_t now = time(NULL);
    int    y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;

    /* ISO 8601 / RFC 3339（允许 T 或空格分隔） */
    if (sscanf(line, "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss) == 6 ||
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
        return TRUE;   /* 无法识别时间戳，不擅自丢弃 */

    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec" };
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
    tmv.tm_mon  = mon_idx;
    tmv.tm_mday = day;
    tmv.tm_hour = hh;
    tmv.tm_min  = mm;
    tmv.tm_sec  = ss;
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

static gboolean pattern_match(const gchar *line, GPtrArray *pats,
                             gboolean regex, gboolean case_sensitive)
{
    if (pats->len == 0)
        return TRUE;

    for (guint i = 0; i < pats->len; i++) {
        const gchar *pat = g_ptr_array_index(pats, i);
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

static gchar *dedup_key(const gchar *line)
{
    const gchar *p = strstr(line, "]: ");

    return g_strdup((p != NULL) ? p + 3 : line);
}

static GString *filter_log(LogPage *page, const gchar *text)
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
        if (!line_matches_level(lines[i], filter))
            continue;

        guint rsel = gtk_drop_down_get_selected(
            GTK_DROP_DOWN(page->range_dd));
        static const gint64 ranges[] = { 0, 3600, 6 * 3600, 24 * 3600 };

        if (rsel < G_N_ELEMENTS(ranges) &&
            !line_in_range(lines[i], ranges[rsel]))
            continue;

        if (!pattern_match(lines[i], inc, regex, FALSE))
            continue;
        if (exc->len > 0 && pattern_match(lines[i], exc, regex, FALSE))
            continue;

        if (dedup) {
            gchar *key = dedup_key(lines[i]);

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
static gboolean at_bottom(LogPage *page)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(page->scroll));

    if (adj == NULL)
        return TRUE;

    return (gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) -
            gtk_adjustment_get_value(adj)) < 32.0;
}

static void apply_pid_tags(GtkTextBuffer *buffer)
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

static gchar *raw_context(LogPage *page, const gchar *needle)
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

static void refilter_only(LogPage *page)
{
    if (page->raw == NULL)
        return;

    gboolean was_at_bottom = at_bottom(page);
    GString *filtered = filter_log(page, page->raw->str);
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *current = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (g_strcmp0(current, filtered->str) != 0) {
        gtk_text_buffer_set_text(buffer, filtered->str, -1);
        apply_pid_tags(buffer);

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

static void show_context_window(const gchar *ctx)
{
    GtkWidget *win = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(win), _("日志上下文（前后 5 行）"));
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 320);
    gtk_window_set_transient_for(GTK_WINDOW(win), fmon_gui_main_window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *view = gtk_text_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *close = gtk_button_new_with_label(_("关闭"));

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             (ctx != NULL) ? ctx : _("（未找到上下文）"), -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_widget_set_margin_end(close, 8);
    gtk_widget_set_margin_bottom(close, 8);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy),
                             win);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), close);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}

typedef struct {
    LogPage *page;
    gchar   *fallback;
    gchar   *key;
    guint    gen;
} CtxFetch;

static void on_ctx_fetch_done(gpointer user_data, const FmonChildResult *res,
                              gboolean ok)
{
    CtxFetch *c = user_data;

    if (fmon_gui_main_window() == NULL) {
        g_object_unref(c->page->root);
        g_free(c->fallback);
        g_free(c->key);
        g_free(c);
        return;
    }

    if (ok && c->gen == c->page->generation && res != NULL &&
        res->out != NULL && res->out[0] != '\0') {
        if (c->page->ctx_cache == NULL)
            c->page->ctx_cache = g_hash_table_new_full(
                g_str_hash, g_str_equal, g_free, g_free);

        if (g_hash_table_size(c->page->ctx_cache) > 200)
            g_hash_table_remove_all(c->page->ctx_cache);

        g_hash_table_replace(c->page->ctx_cache, g_strdup(c->key),
                             g_strdup(res->out));
        show_context_window(res->out);
    } else {
        show_context_window(c->fallback);
    }

    g_object_unref(c->page->root);
    g_free(c->fallback);
    g_free(c->key);
    g_free(c);
}

static void fetch_file_context(LogPage *page, const char *line, guint occ,
                               const char *fallback)
{
    CtxFetch *c = g_new0(CtxFetch, 1);

    c->page = page;
    c->fallback = g_strdup(fallback);
    c->key = g_strdup_printf("%u\x1f%s", occ, line);
    c->gen = page->generation;

    /* grep 定位第 occ 次出现的行号，awk 输出前后 5 行；参数全部按位置传入 */
    const char *script =
        "n=$(grep -n -F -- \"$1\" \"$2\" 2>/dev/null | sed -n \"$3p\" | cut -d: -f1)\n"
        "[ -n \"$n\" ] || n=$(grep -n -F -m 1 -- \"$1\" \"$2\" 2>/dev/null | cut -d: -f1)\n"
        "[ -n \"$n\" ] || exit 3\n"
        "a=$((n > 5 ? n - 5 : 1))\n"
        "b=$((n + 5))\n"
        "awk -v a=$a -v b=$b 'NR>=a && NR<=b' \"$2\"\n";
    gchar *occ_s = g_strdup_printf("%u", occ);
    const char *argv[] = { "/bin/sh", "-c", script, "sh", line,
                           page->path, occ_s, NULL };

    g_object_ref(page->root);
    fmon_gui_run_async_full(argv, 256 * 1024, 5000, on_ctx_fetch_done, c);
    g_free(occ_s);
}

static void on_pid_click(GtkGestureClick *gesture, int n_press, double x,
                         double y, gpointer data)
{
    (void)gesture;
    (void)n_press;

    LogPage *page = data;
    GtkTextView *tv = GTK_TEXT_VIEW(page->text_view);
    GtkTextIter iter;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);

    gtk_text_view_get_iter_at_location(tv, &iter, (int)x, (int)y);

    GtkTextTag *tag = gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(buf), "pid-link");
    gboolean tagged = (tag != NULL) && gtk_text_iter_has_tag(&iter, tag);

    if (!tagged && n_press < 2)   /* 普通单击用于选择文本 */
        return;

    GtkTextIter ls, le;
    int line_no = gtk_text_iter_get_line(&iter);

    gtk_text_buffer_get_iter_at_line(buf, &ls, line_no);
    le = ls;
    gtk_text_iter_forward_to_line_end(&le);

    gchar *line = gtk_text_buffer_get_text(buf, &ls, &le, FALSE);
    int off = gtk_text_iter_get_line_offset(&iter);
    int n = (int)strlen(line);

    if (off >= 0 && off < n) {
        int a = off, b = off;

        while (a > 0 && g_ascii_isdigit(line[a - 1]))
            a--;
        while (b < n && g_ascii_isdigit(line[b]))
            b++;

        if (tagged && b > a) {
            gchar *num = g_strndup(line + a, b - a);
            guint32 pid = (guint32)g_ascii_strtoull(num, NULL, 10);

            g_free(num);

            if (pid > 0) {
                fmon_gui_switch_page("proc");
                gui_process_select_pid(pid);
                g_free(line);
                return;
            }
        }
    }

    g_free(line);

    /* 没点中 PID：弹出该行前后 5 行上下文 */
    GtkTextIter a = iter, b = iter;

    for (int i = 0; i < 5; i++) {
        if (!gtk_text_iter_backward_line(&a))
            break;
    }
    for (int i = 0; i < 5; i++) {
        if (!gtk_text_iter_forward_line(&b))
            break;
    }

    gchar *ctx = raw_context(page, line);

    if (ctx == NULL)
        ctx = gtk_text_buffer_get_text(buf, &a, &b, FALSE);

    if (page->cmd == NULL && page->path != NULL &&
        g_file_test(page->path, G_FILE_TEST_IS_REGULAR)) {
        /* 折叠行带“（重复 N 次）”后缀，定位时去掉后缀再查原文件 */
        gchar *base = g_strdup(line);
        gchar *mark = strstr(base, _("（重复 "));

        if (mark != NULL)
            *mark = '\0';

        g_free(line);
        line = base;

        /* 统计这是当前视图中第几次出现，同名行才能定位到点击的那一处 */
        int clicked_line = gtk_text_iter_get_line(&iter);
        GtkTextIter bs, be;

        gtk_text_buffer_get_start_iter(buf, &bs);
        gtk_text_buffer_get_iter_at_line(buf, &be, clicked_line);
        gtk_text_iter_forward_to_line_end(&be);

        gchar *pre = gtk_text_buffer_get_text(buf, &bs, &be, FALSE);
        gchar **pl = g_strsplit(pre, "\n", -1);
        guint occ = 0;

        for (int i = 0; pl[i] != NULL; i++)
            if (strcmp(pl[i], line) == 0)
                occ++;

        if (occ == 0)
            occ = 1;

        g_strfreev(pl);
        g_free(pre);

        gchar *key = g_strdup_printf("%u\x1f%s", occ, line);
        const gchar *cached = (page->ctx_cache != NULL)
                                  ? g_hash_table_lookup(page->ctx_cache, key)
                                  : NULL;

        if (cached != NULL) {
            show_context_window(cached);
            g_free(key);
            g_free(ctx);
            return;
        }

        g_free(key);
        fetch_file_context(page, line, occ, ctx);
        g_free(ctx);
        return;
    }

    show_context_window(ctx);
    g_free(ctx);
}

static void on_tail_done(gpointer user_data, const FmonChildResult *res,
                         gboolean ok)
{
    LogPage *page = user_data;

    page->busy = FALSE;

    /* 源已切换：丢弃旧结果，并立即按新源重新加载 */
    if (page->req_gen != page->generation) {
        /* 先启动新请求（它会再次持有 root 引用），再释放本次引用 */
        start_tail(page);
        g_object_unref(page->root);
        return;
    }

    /* 回调期间我们持有 root 的引用，因此 page 与控件一定存活 */

    gboolean was_at_bottom = at_bottom(page);

    gchar *raw = NULL;
    if (ok && res != NULL && res->out != NULL)
        raw = fmon_gui_make_valid_utf8(res->out, (gssize)res->out_len);
    else
        raw = g_strdup("");

    if (page->raw == NULL)
        page->raw = g_string_new(NULL);

    if (page->pending == NULL)
        page->pending = g_string_new(NULL);

    if (!page->append) {
        g_string_truncate(page->raw, 0);
        g_string_truncate(page->pending, 0);
    }

    /* 把新字节接到上次残留的半行后面，只把完整行放进 raw */
    g_string_append(page->pending, raw);

    char *last_nl = strrchr(page->pending->str, '\n');

    if (last_nl != NULL) {
        gsize complete = (gsize)(last_nl - page->pending->str) + 1;

        g_string_append_len(page->raw, page->pending->str, complete);
        g_string_erase(page->pending, 0, complete);
    }

    page->append = FALSE;

    /* 控制内存：只保留最后约 1 MB */
    if (page->raw->len > 8 * 1024 * 1024) {
        g_string_erase(page->raw, 0, page->raw->len - 8 * 1024 * 1024);

        /* 从整行边界重新开始，避免上下文首行是半行 */
        char *nl = strchr(page->raw->str, '\n');

        if (nl != NULL)
            g_string_erase(page->raw, 0,
                           (gsize)(nl - page->raw->str) + 1);
    }

    GString *filtered = filter_log(page, page->raw->str);
    g_free(raw);

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *current = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (g_strcmp0(current, filtered->str) != 0) {
        /* 保留滚动位置：只有原本在底部、且处于“继续”状态时才自动滚到底 */
        gtk_text_buffer_set_text(buffer, filtered->str, -1);
        apply_pid_tags(buffer);

        if (page->follow && was_at_bottom) {
            GtkTextIter end_iter;
            gtk_text_buffer_get_end_iter(buffer, &end_iter);
            gtk_text_buffer_place_cursor(buffer, &end_iter);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(page->text_view),
                                         &end_iter, 0.0, FALSE, 0, 0);
        }
    }

    g_free(current);
    g_string_free(filtered, TRUE);

    /* 释放 start_tail() 取得的引用 */
    g_object_unref(page->root);
}

static void on_file_changed(GFileMonitor *mon, GFile *file, GFile *other,
                            GFileMonitorEvent event, gpointer data)
{
    (void)mon;
    (void)file;
    (void)other;

    LogPage *page = data;

    if (event == G_FILE_MONITOR_EVENT_CHANGED ||
        event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event == G_FILE_MONITOR_EVENT_CREATED)
        start_tail(page);
}

static void setup_monitor(LogPage *page)
{
    if (page->monitor != NULL) {
        g_file_monitor_cancel(page->monitor);
        g_clear_object(&page->monitor);
    }

    if (page->path == NULL || page->cmd != NULL ||
        !g_file_test(page->path, G_FILE_TEST_IS_REGULAR))
        return;

    GFile *f = g_file_new_for_path(page->path);

    page->monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);

    if (page->monitor != NULL)
        g_signal_connect(page->monitor, "changed",
                         G_CALLBACK(on_file_changed), page);

    g_object_unref(f);
}

static void start_tail(LogPage *page)
{
    if (page->busy)
        return;

    page->busy = TRUE;
    page->req_gen = page->generation;

    if (page->cmd != NULL) {
        const char *argv[] = { "/bin/sh", "-c", page->cmd, NULL };

        page->append = FALSE;
        page->offset = 0;
        g_object_ref(page->root);
        fmon_gui_run_async_full(argv, 4 * 1024 * 1024, 8000, on_tail_done,
                                page);
    } else {
        struct stat st;

        if (page->path == NULL || stat(page->path, &st) != 0) {
            page->busy = FALSE;
            return;
        }

        goffset size = (goffset)st.st_size;

        /* 原地重写但大小不变时，靠 mtime 使上下文缓存失效 */
        if (page->ctx_cache != NULL && page->initialized &&
            (st.st_mtime != page->file_mtime || size < page->offset))
            g_hash_table_remove_all(page->ctx_cache);

        page->file_mtime = st.st_mtime;

        if (!page->initialized || size < page->offset) {
            if (page->ctx_cache != NULL && size < page->offset)
                g_hash_table_remove_all(page->ctx_cache);
            /* 首次加载或文件被轮转：读最后 400 行 */
            const char *argv[] = { "tail", "-n", "400", page->path, NULL };

            page->append = FALSE;
            page->initialized = TRUE;
            g_object_ref(page->root);
            fmon_gui_run_async_full(argv, 2 * 1024 * 1024, 5000,
                                    on_tail_done, page);
            page->offset = size;
        } else if (size > page->offset) {
            /* 真正的增量：只读新追加的字节 */
            gchar *from = g_strdup_printf("+%" G_GINT64_FORMAT,
                                          (gint64)page->offset + 1);
            const char *argv[] = { "tail", "-c", from, page->path, NULL };

            page->append = TRUE;
            g_object_ref(page->root);
            fmon_gui_run_async_full(argv, 2 * 1024 * 1024, 5000,
                                    on_tail_done, page);
            g_free(from);
            page->offset = size;
        } else {
            page->busy = FALSE;
            return;   /* 没有新内容 */
        }
    }

    gchar *label = g_strdup_printf(_("日志源：%s"),
                                   (page->cmd != NULL) ? page->cmd
                                                       : page->path);
    gtk_label_set_text(GTK_LABEL(page->path_label), label);
    g_free(label);
}

/* ===========================================================================
 * 定时器
 * ========================================================================= */
static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);
    if (id != 0)
        g_source_remove(id);
}

static gboolean on_tick(gpointer data)
{
    LogPage *page = data;

    if (fmon_gui_main_window() == NULL || page->root == NULL)
        return G_SOURCE_REMOVE;

    if (!gtk_widget_get_mapped(page->root))
        return G_SOURCE_CONTINUE;

    if (page->follow)
        start_tail(page);

    return G_SOURCE_CONTINUE;
}

/* ===========================================================================
 * 交互
 * ========================================================================= */
static void on_path_selected(const char *path, LogPage *page)
{
    g_free(page->path);
    g_free(page->cmd);
    page->cmd = NULL;
    page->generation++;
    page->path = g_strdup(path);
    page->offset = 0;
    page->initialized = FALSE;
    if (page->raw != NULL)
        g_string_truncate(page->raw, 0);
    if (page->pending != NULL)
        g_string_truncate(page->pending, 0);
    if (page->ctx_cache != NULL)
        g_hash_table_remove_all(page->ctx_cache);

    setup_monitor(page);

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));
    gtk_text_buffer_set_text(buffer, "", 0);

    start_tail(page);
}

static void on_preset_changed(GObject *dropdown, GParamSpec *pspec,
                              gpointer data)
{
    (void)pspec;

    LogPage *page = data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));

    if (sel == GTK_INVALID_LIST_POSITION)
        return;

    if (sel >= page->sources->len)
        return;   /* 最后一项“自定义…”由按钮处理 */

    LogSource *src = g_ptr_array_index(page->sources, sel);

    g_free(page->path);
    g_free(page->cmd);
    page->path = g_strdup(src->path);
    page->cmd = g_strdup(src->cmd);
    page->generation++;
    page->offset = 0;
    page->initialized = FALSE;
    if (page->raw != NULL)
        g_string_truncate(page->raw, 0);
    if (page->pending != NULL)
        g_string_truncate(page->pending, 0);
    if (page->ctx_cache != NULL)
        g_hash_table_remove_all(page->ctx_cache);

    setup_monitor(page);

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->text_view));
    gtk_text_buffer_set_text(buffer, "", 0);
    start_tail(page);
}

static void on_open_file_done(GObject *source_object, GAsyncResult *result,
                              gpointer user_data)
{
    LogPage *page = user_data;

    if (page->root == NULL)
        return;

    GError *error = NULL;
    GFile  *file  = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object),
                                                result, &error);

    if (file == NULL) {
        if (error != NULL)
            g_error_free(error);
        return;
    }

    gchar *path = g_file_get_path(file);
    if (path != NULL) {
        on_path_selected(path, page);
        g_free(path);
    }
    g_object_unref(file);
}

static void on_open_file(GtkButton *btn, gpointer data)
{
    (void)btn;

    LogPage *page = data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "打开日志文件");
    gtk_file_dialog_set_initial_folder(
        dialog, g_file_new_for_path("/var/log"));

    g_object_ref(dialog);
    gtk_file_dialog_open(dialog, fmon_gui_main_window(), NULL,
                         on_open_file_done, page);
    g_object_unref(dialog);
}

static void on_export_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkTextView *tv = data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);

    if (file == NULL)
        return;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
    GtkTextIter s, e;

    gtk_text_buffer_get_bounds(buf, &s, &e);

    gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    gchar *path = g_file_get_path(file);

    if (path != NULL) {
        g_file_set_contents(path, text, -1, NULL);
        g_free(path);
    }

    g_free(text);
    g_object_unref(file);
}

static void on_export(GtkButton *btn, gpointer data)
{
    (void)btn;

    GtkFileDialog *dlg = gtk_file_dialog_new();

    gtk_file_dialog_set_title(dlg, _("导出当前日志"));
    gtk_file_dialog_set_initial_name(dlg, "fmon-log.txt");
    g_object_ref(dlg);
    gtk_file_dialog_save(dlg, fmon_gui_main_window(), NULL, on_export_done,
                         data);
    g_object_unref(dlg);
}

static void on_reload(GtkButton *btn, gpointer data)
{
    (void)btn;

    LogPage *page = data;

    page->offset = 0;
    page->initialized = FALSE;
    if (page->raw != NULL)
        g_string_truncate(page->raw, 0);
    if (page->pending != NULL)
        g_string_truncate(page->pending, 0);
    start_tail(page);
}

static void on_follow_toggled(GtkToggleButton *btn, gpointer data)
{
    LogPage *page = data;

    page->follow = gtk_toggle_button_get_active(btn);
    gtk_button_set_label(GTK_BUTTON(btn), page->follow ? "暂停" : "继续");

    if (page->follow)
        start_tail(page);
}

static void on_filter_changed(GObject *dropdown, GParamSpec *pspec,
                              gpointer data)
{
    (void)dropdown;
    (void)pspec;
    (void)data;

    /* 有累积数据就直接重过滤，暂停状态下也要立即生效 */
    LogPage *page = data;

    if (page->raw != NULL && page->raw->len > 0)
        refilter_only(page);
    else
        start_tail(page);
}

/* ===========================================================================
 * 日志源
 * ========================================================================= */
static LogSource *source_new(const char *label, const char *path,
                             const char *cmd)
{
    LogSource *s = g_new0(LogSource, 1);

    s->label = g_strdup(label);
    s->path  = (path != NULL) ? g_strdup(path) : NULL;
    s->cmd   = (cmd != NULL) ? g_strdup(cmd) : NULL;
    return s;
}

static void source_free(gpointer data)
{
    LogSource *s = data;

    g_free(s->label);
    g_free(s->path);
    g_free(s->cmd);
    g_free(s);
}

static void add_source(LogPage *page, const char *label, const char *path,
                       const char *cmd)
{
    g_ptr_array_add(page->sources, source_new(label, path, cmd));
}

static void add_file_if_exists(LogPage *page, const char *label,
                               const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        add_source(page, label, path, NULL);
}

static void scan_pkg_logs(LogPage *page)
{
    GDir *d = g_dir_open("/usr/local/var/log", 0, NULL);

    if (d == NULL)
        return;

    const char *name;

    while ((name = g_dir_read_name(d)) != NULL) {
        gchar *path = g_build_filename("/usr/local/var/log", name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            gchar *label = g_strdup_printf("pkg: %s", name);

            add_source(page, label, path, NULL);
            g_free(label);
        }

        g_free(path);
    }

    g_dir_close(d);
}

static void scan_jail_logs(LogPage *page)
{
    static const char *const roots[] = { "/usr/jails", "/usr/local/jails" };

    for (size_t r = 0; r < G_N_ELEMENTS(roots); r++) {
        GDir *d = g_dir_open(roots[r], 0, NULL);

        if (d == NULL)
            continue;

        const char *jail;

        while ((jail = g_dir_read_name(d)) != NULL) {
            gchar *logdir = g_build_filename(roots[r], jail, "var", "log",
                                             NULL);
            GDir  *ld = g_dir_open(logdir, 0, NULL);

            if (ld != NULL) {
                const char *lf;

                while ((lf = g_dir_read_name(ld)) != NULL) {
                    gchar *path = g_build_filename(logdir, lf, NULL);

                    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                        gchar *label = g_strdup_printf(
                            "Jail %s / %s", jail, lf);

                        add_source(page, label, path, NULL);
                        g_free(label);
                    }

                    g_free(path);
                }

                g_dir_close(ld);
            }

            g_free(logdir);
        }

        g_dir_close(d);
    }
}

static void build_sources(LogPage *page)
{
    page->sources = g_ptr_array_new_with_free_func(source_free);

    add_source(page, "系统消息 /var/log/messages", "/var/log/messages", NULL);
    add_file_if_exists(page, "认证 /var/log/auth.log", "/var/log/auth.log");
    add_file_if_exists(page, "安全 /var/log/security", "/var/log/security");
    add_file_if_exists(page, "全部 /var/log/all.log", "/var/log/all.log");
    add_file_if_exists(page, "定时任务 /var/log/cron", "/var/log/cron");
    add_file_if_exists(page, "调试 /var/log/debug.log", "/var/log/debug.log");

    add_source(page, "内核 dmesg（kern.msgbuf）", NULL,
               "dmesg 2>/dev/null | tail -n 400");

    if (g_find_program_in_path("zpool") != NULL)
        add_source(page, "ZFS 事件（zpool events -v）", NULL,
                   "zpool events -v 2>/dev/null | tail -n 400");

    scan_pkg_logs(page);
    scan_jail_logs(page);
}
static void log_page_free(gpointer data)
{
    LogPage *page = data;

    if (page == NULL)
        return;

    g_free(page->path);
    g_free(page->cmd);
    if (page->raw != NULL)
        g_string_free(page->raw, TRUE);
    if (page->pending != NULL)
        g_string_free(page->pending, TRUE);
    if (page->ctx_cache != NULL)
        g_hash_table_destroy(page->ctx_cache);
    if (page->monitor != NULL) {
        g_signal_handlers_disconnect_by_data(page->monitor, page);
        g_file_monitor_cancel(page->monitor);
        g_object_unref(page->monitor);
    }
    if (page->sources != NULL)
        g_ptr_array_free(page->sources, TRUE);
    g_free(page);
}

GtkWidget *gui_log_view_new(void)
{
    LogPage *page = g_new0(LogPage, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(vbox, "view-card");
    gtk_widget_set_vexpand(vbox, TRUE);
    page->root = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *filterbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn_reload = gtk_button_new_with_label(_("刷新"));
    gtk_widget_add_css_class(btn_reload, "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), btn_reload);

    GtkWidget *btn_open = gtk_button_new_with_label(_("打开文件…"));
    gtk_box_append(GTK_BOX(toolbar), btn_open);

    build_sources(page);

    GtkStringList *presets = gtk_string_list_new(NULL);

    for (guint i = 0; i < page->sources->len; i++) {
        LogSource *s = g_ptr_array_index(page->sources, i);

        gtk_string_list_append(presets, s->label);
    }

    gtk_string_list_append(presets, _("自定义…"));

    page->preset_dd = gtk_drop_down_new(G_LIST_MODEL(presets), NULL);
    g_object_unref(presets);
    gtk_box_append(GTK_BOX(toolbar), page->preset_dd);

    GtkStringList *levels = gtk_string_list_new(NULL);
    gtk_string_list_append(levels, "全部级别");
    gtk_string_list_append(levels, "紧急");
    gtk_string_list_append(levels, "警报");
    gtk_string_list_append(levels, "严重");
    gtk_string_list_append(levels, "错误");
    gtk_string_list_append(levels, "警告");
    gtk_string_list_append(levels, "通知");
    gtk_string_list_append(levels, "信息");
    gtk_string_list_append(levels, "调试");

    page->level_dd = gtk_drop_down_new(G_LIST_MODEL(levels), NULL);
    g_object_unref(levels);
    gtk_box_append(GTK_BOX(toolbar), page->level_dd);

    GtkStringList *ranges = gtk_string_list_new(NULL);
    gtk_string_list_append(ranges, "全部时间");
    gtk_string_list_append(ranges, "1 小时");
    gtk_string_list_append(ranges, "6 小时");
    gtk_string_list_append(ranges, "24 小时");
    page->range_dd = gtk_drop_down_new(G_LIST_MODEL(ranges), NULL);
    g_object_unref(ranges);
    gtk_box_append(GTK_BOX(toolbar), page->range_dd);

    page->search = gtk_search_entry_new();
    gtk_widget_set_hexpand(page->search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->search),
                                           _("搜索（支持 -排除词）"));
    gtk_widget_set_size_request(page->search, 220, -1);
    gtk_box_append(GTK_BOX(filterbar), page->search);

    page->exclude = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->exclude),
                                           _("排除关键词"));
    gtk_widget_set_size_request(page->exclude, 140, -1);
    gtk_box_append(GTK_BOX(filterbar), page->exclude);

    page->regex_btn = gtk_toggle_button_new_with_label(".*");
    gtk_widget_set_tooltip_text(page->regex_btn, _("按正则表达式匹配"));
    gtk_box_append(GTK_BOX(filterbar), page->regex_btn);

    page->dedup_btn = gtk_toggle_button_new_with_label(_("折叠重复"));
    gtk_box_append(GTK_BOX(filterbar), page->dedup_btn);

    page->follow_btn = gtk_toggle_button_new_with_label(_("暂停"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->follow_btn), TRUE);
    gtk_box_append(GTK_BOX(toolbar), page->follow_btn);

    GtkWidget *btn_export = gtk_button_new_with_label(_("导出"));

    gtk_box_append(GTK_BOX(toolbar), btn_export);

    gtk_box_append(GTK_BOX(vbox), toolbar);
    gtk_box_append(GTK_BOX(vbox), filterbar);

    page->path_label = gtk_label_new(_("日志文件：/var/log/messages"));
    gtk_widget_add_css_class(page->path_label, "small-text");
    gtk_widget_set_halign(page->path_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), page->path_label);

    page->scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(page->scroll, TRUE);

    page->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(page->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(page->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(page->text_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start(page->text_view, 8);
    gtk_widget_set_margin_end(page->text_view, 8);
    gtk_widget_set_margin_top(page->text_view, 8);
    gtk_widget_set_margin_bottom(page->text_view, 8);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(page->scroll),
                                  page->text_view);
    gtk_box_append(GTK_BOX(vbox), page->scroll);

    GtkGesture *click = gtk_gesture_click_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pid_click), page);
    gtk_widget_add_controller(page->text_view,
                              GTK_EVENT_CONTROLLER(click));

    page->path     = g_strdup("/var/log/messages");
    page->follow   = TRUE;
    setup_monitor(page);

    g_object_set_data_full(G_OBJECT(vbox), "log-page", page, log_page_free);

    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload), page);
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_file), page);
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_export),
                     page->text_view);
    g_signal_connect(page->preset_dd, "notify::selected",
                     G_CALLBACK(on_preset_changed), page);
    g_signal_connect(page->exclude, "search-changed",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->regex_btn, "toggled",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->dedup_btn, "toggled",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->level_dd, "notify::selected",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->range_dd, "notify::selected",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_filter_changed), page);
    g_signal_connect(page->follow_btn, "toggled",
                     G_CALLBACK(on_follow_toggled), page);

    /* 定时器与控件同生命周期：控件销毁时自动移除 */
    page->timer = g_timeout_add_seconds(3, on_tick, page);
    g_object_set_data_full(G_OBJECT(vbox), "log-timer",
                           GUINT_TO_POINTER(page->timer), remove_source);

    start_tail(page);

    return vbox;
}
