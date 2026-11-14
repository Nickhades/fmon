/*
 * gui_nav_pages.h - 导航页面表与分组表
 */

#ifndef FMON_GUI_NAV_PAGES_H
#define FMON_GUI_NAV_PAGES_H

typedef struct {
    const char *id;
    const char *title;
    const char *icon;
    const char *desc;
} GuiNavPage;

typedef struct {
    const char *title;
    const char *icon;
    const char *desc;
    int         pages[12];
    int         n_pages;
} GuiNavGroup;

int gui_nav_group_count(void);
const GuiNavGroup *gui_nav_group(int gi);

int gui_nav_page_count(void);
const GuiNavPage *gui_nav_page(int idx);
int gui_nav_find_page(const char *id);
int gui_nav_group_of_page(int idx);

#endif /* FMON_GUI_NAV_PAGES_H */
