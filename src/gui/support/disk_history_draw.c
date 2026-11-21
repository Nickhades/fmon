/*
 * disk_history_draw.c - 磁盘历史曲线绘制
 */

#include "disk_history_internal.h"

#include "theme.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>
#include <time.h>

#define HIST_WINDOW_SEC (24 * 3600)

void disk_history_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                         gpointer data)
{
    (void)area;

    HistDlg *d = data;

    int metric = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->drop));

    if (metric < 0)
        metric = 0;

    if (d->pts != NULL) {
        g_array_free(d->pts, TRUE);
        d->pts = NULL;
    }

    GArray *pts = disk_history_query_points(d->dev, metric);

    d->pts = pts;

    double axis[4], grid[4];

    fmon_theme_axis_color(axis);
    fmon_theme_grid_color(grid, FALSE);

    double ml = 54.0, mr = 14.0, mt = 26.0, mb = 30.0;
    double pw = w - ml - mr;
    double ph = h - mt - mb;

    if (pw < 10.0 || ph < 10.0)
        return;

    d->ml = ml;
    d->mt = mt;
    d->pw = pw;
    d->ph = ph;

    gint64 now   = (gint64)time(NULL);
    gint64 since = now - HIST_WINDOW_SEC;

    /* 历史还不足一天时自动放大到已有数据，否则几个点会挤在右边缘 */
    if (pts->len > 0) {
        gint64 first = (gint64)g_array_index(pts, DiskHistoryPoint, 0).t;

        if (first > since + 6 * 3600)
            since = first - 600;
    }

    double window = (double)(now - since);

    double r = 0.30, g = 0.76, b = 1.00;
    const char *unit = "KB/s";

    if (metric == 0) {
        r = 0.95; g = 0.60; b = 0.20; unit = "ms";
    } else if (metric == 2) {
        r = 0.95; g = 0.30; b = 0.30; unit = "°C";
    }

    cairo_set_line_width(cr, 1.0);

    for (int i = 0; i <= 4; i++) {
        double y = mt + ph * (double)i / 4.0;

        cairo_set_source_rgba(cr, grid[0], grid[1], grid[2], grid[3]);
        cairo_move_to(cr, ml, y);
        cairo_line_to(cr, ml + pw, y);
        cairo_stroke(cr);
    }

    if (pts->len == 0) {
        if (d->stats != NULL)
            gtk_label_set_text(GTK_LABEL(d->stats),
                               _("暂无明细数据（运行 1 分钟后开始记录）"));

        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], axis[3]);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, ml + 12.0, mt + ph / 2.0);
        cairo_show_text(cr, "暂无历史数据（运行 1 分钟后开始记录）");
        return;
    }

    double vmin = pts->len ? g_array_index(pts, DiskHistoryPoint, 0).v : 0.0;
    double vmax = vmin;

    for (guint i = 1; i < pts->len; i++) {
        double v = g_array_index(pts, DiskHistoryPoint, i).v;

        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    if (vmax - vmin < 1e-9) {
        vmin -= 1.0;
        vmax += 1.0;
    } else {
        double pad = (vmax - vmin) * 0.10;

        vmin -= pad;
        vmax += pad;
    }

    if (vmin < 0.0)
        vmin = 0.0;

    d->since  = since;
    d->window = window;
    d->vmin   = vmin;
    d->vmax   = vmax;

    /* 明细统计：最新 / 最小 / 最大 / 平均 / 点数 */
    {
        double sum = 0.0, lo = 1e18, hi = -1e18;

        for (guint i = 0; i < pts->len; i++) {
            double v = g_array_index(pts, DiskHistoryPoint, i).v;

            sum += v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }

        double latest = g_array_index(pts, DiskHistoryPoint, pts->len - 1).v;
        gchar *st = g_strdup_printf(
            _("最新 %.2f %s · 最小 %.2f · 最大 %.2f · 平均 %.2f · %u 个采样点"),
            latest, unit, lo, hi, sum / (double)pts->len, pts->len);

        if (d->stats != NULL)
            gtk_label_set_text(GTK_LABEL(d->stats), st);
        g_free(st);
    }

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);

    for (int i = 0; i <= 4; i++) {
        double v = vmax - (vmax - vmin) * (double)i / 4.0;
        char   buf[32];

        g_snprintf(buf, sizeof(buf), "%.2f %s", v, unit);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.85);
        cairo_text_extents_t ext;

        cairo_text_extents(cr, buf, &ext);
        cairo_move_to(cr, ml - 6.0 - ext.width, mt + ph * (double)i / 4.0 + 3.0);
        cairo_show_text(cr, buf);
    }

    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.85);
    char left[32];

    if (window >= 20 * 3600)
        g_strlcpy(left, "24 小时前", sizeof(left));
    else if (window >= 3600)
        g_snprintf(left, sizeof(left), "%.0f 小时前", window / 3600.0);
    else
        g_snprintf(left, sizeof(left), "%.0f 分钟前", window / 60.0);

    cairo_move_to(cr, ml, mt + ph + 18.0);
    cairo_show_text(cr, left);
    cairo_move_to(cr, ml + pw - 32.0, mt + ph + 18.0);
    cairo_show_text(cr, "现在");

    cairo_set_source_rgba(cr, r, g, b, 1.0);
    cairo_set_line_width(cr, 1.2);

    gboolean first = TRUE;
    double   last_y = 0.0;

    for (guint i = 0; i < pts->len; i++) {
        DiskHistoryPoint p = g_array_index(pts, DiskHistoryPoint, i);
        double x = ml + pw * (p.t - (double)since) / window;
        double y = mt + ph - ph * (p.v - vmin) / (vmax - vmin);

        if (x < ml) x = ml;
        if (x > ml + pw) x = ml + pw;
        if (y < mt) y = mt;
        if (y > mt + ph) y = mt + ph;

        if (first) {
            cairo_move_to(cr, x, y);
            first = FALSE;
        } else {
            cairo_line_to(cr, x, last_y);
            cairo_line_to(cr, x, y);
        }
        last_y = y;
    }

    cairo_stroke(cr);

    /* 只有一个采样点时画一个圆点，否则什么都看不到 */
    if (pts->len == 1) {
        DiskHistoryPoint p = g_array_index(pts, DiskHistoryPoint, 0);
        double x = ml + pw * (p.t - (double)since) / window;
        double y = mt + ph - ph * (p.v - vmin) / (vmax - vmin);

        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_arc(cr, x, y, 3.0, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    /* 悬停：显示最近采样点的时间与值 */
    if (d->hover && pts->len > 0) {
        double best = 1e18;
        guint  bi   = 0;

        for (guint i = 0; i < pts->len; i++) {
            DiskHistoryPoint p = g_array_index(pts, DiskHistoryPoint, i);
            double x = ml + pw * (p.t - (double)since) / window;
            double dd = fabs(x - d->hover_x);

            if (dd < best) {
                best = dd;
                bi   = i;
            }
        }

        DiskHistoryPoint p = g_array_index(pts, DiskHistoryPoint, bi);
        double x = ml + pw * (p.t - (double)since) / window;
        double y = mt + ph - ph * (p.v - vmin) / (vmax - vmin);

        if (x < ml) x = ml;
        if (x > ml + pw) x = ml + pw;
        if (y < mt) y = mt;
        if (y > mt + ph) y = mt + ph;

        cairo_save(cr);
        double dashes[] = { 3.0, 3.0 };

        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.55);
        cairo_move_to(cr, x, mt);
        cairo_line_to(cr, x, mt + ph);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);

        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_arc(cr, x, y, 3.5, 0, 2 * M_PI);
        cairo_fill(cr);

        struct tm tmv;
        time_t    tt = (time_t)p.t;

        localtime_r(&tt, &tmv);

        char hhmm[16], txt[72];

        strftime(hhmm, sizeof(hhmm), "%H:%M", &tmv);
        g_snprintf(txt, sizeof(txt), "%s  %.2f %s", hhmm, p.v, unit);

        cairo_text_extents_t ext;

        cairo_text_extents(cr, txt, &ext);

        double tx = x + 8.0;

        if (tx + ext.width > ml + pw)
            tx = x - 8.0 - ext.width;

        cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.95);
        cairo_move_to(cr, tx, mt + 14.0);
        cairo_show_text(cr, txt);
        cairo_restore(cr);
    }

    cairo_set_source_rgba(cr, axis[0], axis[1], axis[2], 0.7);
    cairo_set_font_size(cr, 9.0);
    cairo_move_to(cr, ml + 4.0, mt - 8.0);
    cairo_show_text(cr, "每分钟平均 · 保留 7 天");
}
