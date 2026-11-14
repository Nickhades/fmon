/*
 * fmon_sysctl.h - 守护进程内部使用的 sysctl / 时间 小工具
 *
 * 只有头文件：所有函数都是 static inline，不产生额外目标文件。
 */

#ifndef FMON_SYSCTL_H
#define FMON_SYSCTL_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FMON_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* 当前单调时钟（微秒）。 */
static inline uint64_t fmon_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline int64_t fmon_now_ms(void)
{
    return (int64_t)(fmon_now_us() / 1000u);
}

/*
 * 读取一个 sysctl 到 uint64_t。
 * 内核返回的实际长度可能小于 sizeof(uint64_t)（例如 u_int），
 * 由于缓冲区提前清零，短读也是安全的。
 */
static inline int fmon_sysctl_u64(const char *name, uint64_t *out)
{
    uint64_t v = 0;
    size_t len = sizeof(v);

    if (out == NULL)
        return -1;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return -1;
    if (len == 0 || len > sizeof(v))
        return -1;
    *out = v;
    return 0;
}

static inline int fmon_sysctl_int(const char *name, int *out)
{
    int v = 0;
    size_t len = sizeof(v);

    if (out == NULL)
        return -1;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return -1;
    if (len != sizeof(v))
        return -1;
    *out = v;
    return 0;
}

static inline int fmon_sysctl_uint(const char *name, unsigned int *out)
{
    unsigned int v = 0;
    size_t len = sizeof(v);

    if (out == NULL)
        return -1;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return -1;
    if (len != sizeof(v))
        return -1;
    *out = v;
    return 0;
}

/* 读取字符串型 sysctl，并保证以 '\0' 结尾。 */
static inline int fmon_sysctl_str(const char *name, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0)
        return -1;

    buf[0] = '\0';
    size_t len = buf_len;
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0)
        return -1;
    buf[buf_len - 1] = '\0';
    return 0;
}

/* 可被信号打断的毫秒睡眠：收到信号时立即返回 false。 */
static inline bool fmon_sleep_ms(int ms)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    rem.tv_sec = 0;
    rem.tv_nsec = 0;

    while (nanosleep(&req, &rem) != 0) {
        if (errno == EINTR) {
            req = rem;
            return false;
        }
        return true;
    }
    return true;
}

#endif /* FMON_SYSCTL_H */
