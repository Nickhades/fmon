/*
 * theme.h - 浅色 / 深色主题管理
 *
 * 三种模式：
 *   FMON_THEME_AUTO  - 跟随桌面环境（GTK 颜色方案）；
 *   FMON_THEME_LIGHT - 强制浅色；
 *   FMON_THEME_DARK  - 强制深色。
 *
 * 选择会写入 $XDG_CONFIG_HOME/fmon/theme，下次启动自动恢复。
 * 同时给 Cairo 绘制代码提供与主题匹配的配色。
 */

#ifndef FMON_THEME_H
#define FMON_THEME_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMON_THEME_AUTO  = 0,
    FMON_THEME_LIGHT = 1,
    FMON_THEME_DARK  = 2
} FmonThemeMode;

/* 读取持久化设置并应用；必须在创建窗口之前调用。 */
void fmon_theme_init(void);

/* 切换模式：立即应用、写盘，并通知观察者。 */
void fmon_theme_set(FmonThemeMode mode);

FmonThemeMode fmon_theme_mode(void);

/* 当前实际是否深色（AUTO 时取系统颜色方案）。 */
gboolean fmon_theme_is_dark(void);

/* 主题变化回调：界面据此重绘曲线等自绘控件。 */
void fmon_theme_set_observer(void (*cb)(void));

/* ---- Cairo 配色（RGBA，0..1） ---- */

void fmon_theme_panel_colors(double top[4], double bottom[4]);
void fmon_theme_texture_color(double rgba[4]);
void fmon_theme_grid_color(double rgba[4], gboolean separator);
void fmon_theme_axis_color(double rgba[4]);
void fmon_theme_hover_line(double rgba[4]);
void fmon_theme_hover_bg(double rgba[4]);

#ifdef __cplusplus
}
#endif

#endif /* FMON_THEME_H */
