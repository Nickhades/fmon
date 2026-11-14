#ifndef FMON_DISK_HISTORY_H
#define FMON_DISK_HISTORY_H

#include <gtk/gtk.h>

/* 每分钟聚合磁盘延迟/吞吐/温度到 SQLite，保留 7 天。
 * 采样与写库都在后台，不阻塞主循环。 */
void disk_history_start(void);

/* 打开某块设备的 24 小时历史窗口（延迟 / 吞吐 / 温度） */
void disk_history_open_dialog(const char *dev, GtkWindow *parent);

#endif /* FMON_DISK_HISTORY_H */
