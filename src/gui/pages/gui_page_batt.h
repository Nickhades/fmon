#ifndef FMON_GUI_PAGE_BATT_H
#define FMON_GUI_PAGE_BATT_H

#include <gtk/gtk.h>

/* 电池健康与电气细节（acpiconf -i 0）；没有电池时显示不可用 */
GtkWidget *gui_batt_detail_box(void);
void       gui_batt_detail_update(void);

#endif /* FMON_GUI_PAGE_BATT_H */
