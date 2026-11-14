/*
 * gui_page_build.h - 页面骨架构建器
 */

#ifndef FMON_GUI_PAGE_BUILD_H
#define FMON_GUI_PAGE_BUILD_H

#include <gtk/gtk.h>

#include "gui_pages.h"
#include "../charts/combo_chart.h"

GtkWidget *gui_page_scrolled(GtkWidget *child);

/* compact = TRUE 时，键值对放在标题行右侧（不占底部一块），
 * 用于 ZFS 这类“曲线 + 一排数字”的页面。 */
GtkWidget *gui_metric_page_new(MetricPage *mp, int idx, const char *title,
                               const char *subtitle,
                               const char *const *keys, int nkeys,
                               void (*update)(MetricPage *p),
                               gboolean compact);

GtkWidget *gui_combo_page_new(const char *title, const char *subtitle,
                              ComboChart *chart, const char *const *rows,
                              int nrows, ComboPage *page);

#endif /* FMON_GUI_PAGE_BUILD_H */
