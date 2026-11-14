/*
 * font_setup.h - 自带的字体渲染设置
 *
 * 目标：不依赖桌面环境（GNOME/KDE 的 fontconfig 配置、Xft 资源）来决定
 * 本程序的字体渲染。启动时：
 *   1. 强制使用 CPU（Cairo）渲染器；
 *   2. 向 fontconfig 注入一组明确的渲染参数（抗锯齿、次像素、hinting）；
 *   3. 让 sans-serif 优先选择 Noto Sans，避免落到低质量的备用字体。
 */

#ifndef FONT_SETUP_H
#define FONT_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* 必须在 gtk_application_new()/gtk_init() 之前调用 */
void fmon_font_setup(void);

#ifdef __cplusplus
}
#endif

#endif /* FONT_SETUP_H */
