/*
 * cpu_health.c - CPU 健康判定
 *
 * 阈值面向桌面/工作站：
 *   温度 >= 95C 或 1 分钟平均使用率 >= 95% 或 负载 > 线程数  -> 异常
 *   温度 >= 85C 或 1 分钟平均使用率 >= 80% 或 负载 > 线程数   -> 注意
 * 负载按“超过线程数”判定拥堵，避免 16 线程机器上 1.5 的负载被误报。
 */

#include "cpu_health.h"

#include <stdio.h>

CpuHealth cpu_health_eval(double usage1m, double load1, unsigned ncpu,
                          double temp_c, char *msg, size_t msglen)
{
    const char *state = "设备正常";
    CpuHealth   h     = CPU_HEALTH_OK;

    if (temp_c >= 95.0) {
        h     = CPU_HEALTH_BAD;
        state = "异常：CPU 温度过高";
    } else if (usage1m >= 95.0) {
        h     = CPU_HEALTH_BAD;
        state = "异常：CPU 长期满载";
    } else if (ncpu > 0 && load1 > (double)ncpu * 2.0) {
        h     = CPU_HEALTH_BAD;
        state = "异常：系统负载过高";
    } else if (temp_c >= 85.0) {
        h     = CPU_HEALTH_WARN;
        state = "注意：CPU 温度偏高";
    } else if (usage1m >= 80.0) {
        h     = CPU_HEALTH_WARN;
        state = "注意：CPU 使用率偏高";
    } else if (ncpu > 0 && load1 > (double)ncpu) {
        h     = CPU_HEALTH_WARN;
        state = "注意：系统负载偏高";
    }

    if (msg != NULL && msglen > 0) {
        if (h == CPU_HEALTH_OK)
            snprintf(msg, msglen, "%s · 使用率 %.0f%% · 负载 %.2f · %.0f°C",
                     state, usage1m, load1, temp_c);
        else
            snprintf(msg, msglen, "%s · 使用率 %.0f%% · 负载 %.2f · %.0f°C",
                     state, usage1m, load1, temp_c);
    }

    return h;
}
