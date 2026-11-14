/*
 * gui_state.h - 界面共享的数据快照访问接口
 *
 * 所有页面都通过这里读取数据：拿到的是 main.c 在每次刷新时从共享内存
 * 一次性拷贝出来的一致快照，而不是直接读映射内存。
 */

#ifndef GUI_STATE_H
#define GUI_STATE_H

#include <gtk/gtk.h>

#include "fmon_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最近一次一致快照（永远非 NULL，数据可能为全 0） */
const FmonShmData *gui_state_snapshot(void);

/* 守护进程是否在线且数据新鲜 */
gboolean gui_state_online(void);

/* 当前选中的 GPU 序号（0..gpu_count-1） */
int  gui_state_gpu_index(void);
void gui_state_set_gpu_index(int index);

#ifdef __cplusplus
}
#endif

#endif /* GUI_STATE_H */
