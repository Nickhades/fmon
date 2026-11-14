/*
 * collector_thread.h - GUI 常驻采集线程
 *
 * 把 zfs/cpu/net/disk 四个 sysctl 类采集从主线程搬到单个常驻线程，
 * 每帧采集完成后经主循环派发，整帧发布（双缓冲思路），主线程只更新 UI。
 * 设置 FMON_LEGACY_COLLECT=1 可退回旧路径（主线程/GTask）。
 */

#ifndef GUI_COLLECTOR_THREAD_H
#define GUI_COLLECTOR_THREAD_H

#include <glib.h>

/* 第一帧发布后回调（主线程）：用于首屏立即刷新/显示窗口 */
typedef void (*GuiCollectorReadyFn)(void);
void     gui_collector_set_ready_cb(GuiCollectorReadyFn fn);

void     gui_collector_start(void);
void     gui_collector_stop(void);

/* TRUE 表示常驻线程已接管采集，刷新循环不要再调用旧 update */
gboolean gui_collector_active(void);

#endif /* GUI_COLLECTOR_THREAD_H */
