/*
 * fmon_daemon.h - 守护进程内部共享的声明
 */

#ifndef FMON_DAEMON_H
#define FMON_DAEMON_H

#include <signal.h>
#include <stdint.h>

#include "fmon_shm.h"

/* 指向共享内存的写者视图，仅守护进程使用 */
extern FmonShmData *g_shm;

/* 由信号处理函数置 0，各采集线程据此退出 */
extern volatile sig_atomic_t g_running;

#include "collectors.h"

#endif /* FMON_DAEMON_H */
