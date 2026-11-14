/*
 * alerts.h - 阈值告警与桌面通知
 *
 * 设计：
 *   - 阈值保存在 ~/.config/fmon/alerts.ini（不存在时用默认值）；
 *   - 每秒检查一次快照，超过阈值时通过 notify-send 发桌面通知；
 *   - 每个指标有 10 分钟冷却，避免通知刷屏；
 *   - 没有 notify-send 时只更新界面上的告警提示，不弹窗。
 */

#ifndef ALERTS_H
#define ALERTS_H

#include <gtk/gtk.h>

#include "fmon_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gboolean enabled;

    double cpu_pct;      /* CPU 使用率 >= 该值告警 */
    double mem_pct;      /* 物理内存使用率 */
    double swap_pct;     /* Swap 使用率 */
    double pool_pct;     /* ZFS 池使用率 */
    double temp_c;       /* CPU 温度 */
    double battery_pct;  /* 电池低于该值告警 */
} AlertSettings;

/* 告警级别 */
typedef enum {
    ALERT_LEVEL_WARNING = 0,
    ALERT_LEVEL_CRITICAL
} AlertLevel;

/* 静默：minutes <= 0 表示取消静默 */
void        alerts_silence(int minutes);
gboolean    alerts_is_silenced(void);
int         alerts_silenced_minutes_left(void);

/* 告警历史文件路径（~/.local/state/fmon/alerts.log） */
const gchar *alerts_log_path(void);

/* 读取/保存设置（文件不存在时使用默认值，不会报错） */
void alerts_load(AlertSettings *settings);
void alerts_save(const AlertSettings *settings);

/*
 * 检查一次快照。
 *   - 对“刚越过阈值”的指标发送桌面通知（10 分钟内同一指标只发一次）；
 *   - 返回当前仍处于告警状态的摘要字符串（调用者 g_free），无告警返回 NULL。
 */
gchar *alerts_check(const FmonShmData *snap, const AlertSettings *settings);

/* 创建“告警”按钮：点击后打开阈值设置对话框（修改会立即保存）。 */
GtkWidget *alerts_create_button(GtkWindow *parent, AlertSettings *settings);

/* 发送一条测试通知（用于确认桌面通知可用） */
void alerts_test_notify(void);

#ifdef __cplusplus
}
#endif

#endif /* ALERTS_H */
