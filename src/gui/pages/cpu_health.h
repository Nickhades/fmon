/*
 * cpu_health.h - CPU 健康判定（纯函数，可单测）
 */

#ifndef FMON_CPU_HEALTH_H
#define FMON_CPU_HEALTH_H

#include <stddef.h>

typedef enum {
    CPU_HEALTH_OK = 0,
    CPU_HEALTH_WARN,
    CPU_HEALTH_BAD,
} CpuHealth;

/* 依据 1 分钟平均使用率、1 分钟负载、线程数、温度给出结论与说明 */
CpuHealth cpu_health_eval(double usage1m, double load1, unsigned ncpu,
                          double temp_c, char *msg, size_t msglen);

#endif /* FMON_CPU_HEALTH_H */
