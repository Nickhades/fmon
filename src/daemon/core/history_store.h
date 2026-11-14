/*
 * history_store.h - 指标历史持久化
 *
 * 守护进程每 10 秒把一行指标追加到 TSV 文件，供界面查看 1 小时 / 24 小时历史：
 *   epoch  cpu%  mem%  swap%  net_rx  net_tx  disk_read  disk_write  temp
 *
 * 路径：
 *   root  : /var/db/fmon/history.tsv
 *   普通用户（本地调试）: $HOME/.local/state/fmon/history.tsv
 */

#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

#include "fmon_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 追加一条采样（由发布线程每 10 秒调用一次） */
void fmon_history_store_sample(const FmonShmData *s);

/* 历史文件路径（只读，供界面显示） */
const char *fmon_history_store_path(void);

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_STORE_H */
