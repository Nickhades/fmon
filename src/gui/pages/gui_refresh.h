/*
 * gui_refresh.h - 页面刷新
 */

#ifndef FMON_GUI_REFRESH_H
#define FMON_GUI_REFRESH_H

#include "fmon_shm.h"

void gui_pages_refresh(const FmonShmData *snap);
void gui_pages_redraw_all(void);

#endif /* FMON_GUI_REFRESH_H */
