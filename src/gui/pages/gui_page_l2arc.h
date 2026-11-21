/*
 * gui_page_l2arc.h - L2ARC 明细页
 */

#ifndef FMON_GUI_PAGE_L2ARC_H
#define FMON_GUI_PAGE_L2ARC_H

#include <gtk/gtk.h>

#include "gui_chart.h"

GtkWidget *gui_l2arc_page_new(void);
void       gui_l2arc_update(void);

CurveData *gui_l2arc_size_curve(void);
CurveData *gui_l2arc_hit_curve(void);

#endif /* FMON_GUI_PAGE_L2ARC_H */
