/*
 * collectors.h - 各采集线程的入口声明
 *
 * 每个采集器是一个独立 .c 文件；这里只集中声明线程入口，
 * 具体实现不共享任何状态（只通过 g_shm + 写锁交互）。
 */

#ifndef FMON_DAEMON_COLLECTORS_H
#define FMON_DAEMON_COLLECTORS_H

#include "fmon_shm.h"

void *thread_cpu_mem(void *arg);   /* CPU / 每核 / 温度 / 内存 / 负载 / 电池 */
void *thread_io(void *arg);        /* 磁盘 I/O（devstat）+ GPU 探测 */
void *thread_net(void *arg);       /* 网卡速率与地址 */
void *thread_zfs(void *arg);       /* zpool/ARC/命中率/带宽/Prune */
void *thread_meminfo(void *arg);   /* 内存条（dmidecode） */
void *thread_storage(void *arg);   /* 文件系统挂载点 + Swap */
void *thread_procs(void *arg);     /* 进程快照（kvm）*/
void *thread_publisher(void *arg); /* 历史推进 + 心跳（唯一发布者）*/

/* GPU 探测 + 负载采集（内部管理写锁，可从任意采集线程调用） */
void fmon_gpu_collect(FmonShmData *s);

#endif /* FMON_DAEMON_COLLECTORS_H */
