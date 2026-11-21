/*
 * fmon_child.c - posix_spawn 方式执行子进程（无 shell）
 *
 * 用 posix_spawn 代替 fork+exec：GTK 进程是多线程的，fork 后子进程
 * 可能继承其它线程持有的锁（malloc/stdio 等），存在死锁风险；
 * posix_spawn 由内核/实现安全创建进程，只做 exec。
 */

#include "fmon_child.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void fmon_child_result_free(FmonChildResult *res)
{
    if (res == NULL)
        return;
    free(res->out);
    res->out      = NULL;
    res->out_len  = 0;
}

bool fmon_child_ok(const FmonChildResult *res)
{
    if (res == NULL || res->status < 0)
        return false;
    return WIFEXITED(res->status) && WEXITSTATUS(res->status) == 0;
}

static bool buf_append(char **buf, size_t *len, size_t *cap, size_t limit,
                       bool *truncated, const char *data, size_t n)
{
    if (*len >= limit) {
        *truncated = true;
        return true;
    }
    if (n > limit - *len)
        n = limit - *len;

    if (*len + n + 1 > *cap) {
        size_t want = (*cap == 0) ? 8192 : *cap;
        while (want < *len + n + 1)
            want *= 2;
        char *nbuf = realloc(*buf, want);
        if (nbuf == NULL)
            return false;
        *buf = nbuf;
        *cap = want;
    }

    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

static int child_run_impl(const char *const argv[], size_t max_bytes,
                          int timeout_ms, bool merge_stderr,
                          FmonChildResult *res)
{
    if (res == NULL)
        return -1;

    memset(res, 0, sizeof(*res));
    res->status = -1;

    if (argv == NULL || argv[0] == NULL) {
        res->error = EINVAL;
        return -1;
    }

    if (max_bytes == 0)
        max_bytes = FMON_CHILD_DEFAULT_MAX;
    if (timeout_ms <= 0)
        timeout_ms = FMON_CHILD_DEFAULT_TIMEOUT_MS;

    int pfd[2];
    if (pipe(pfd) != 0) {
        res->error = errno;
        return -1;
    }

    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);

    if (rc != 0) {
        res->error = rc;
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);

    if (merge_stderr)
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDERR_FILENO);
    else
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0);

    /* 子进程不需要读端和已复制的写端 */
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);

    extern char **environ;
    pid_t pid = 0;

    rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv,
                      environ);

    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        res->error = rc;
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    (void)close(pfd[1]);

    char  *buf = NULL;
    size_t len = 0, cap = 0;
    bool   io_failed = false;
    int64_t deadline = now_ms() + timeout_ms;

    for (;;) {
        int64_t remain = deadline - now_ms();
        if (remain < 0) {
            res->timed_out = true;
            (void)kill(pid, SIGKILL);
            break;
        }

        struct pollfd p = { .fd = pfd[0], .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, (int)remain);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            io_failed = true;
            break;
        }
        if (pr == 0) {
            res->timed_out = true;
            (void)kill(pid, SIGKILL);
            break;
        }

        char tmp[4096];
        ssize_t n = read(pfd[0], tmp, sizeof(tmp));
        if (n > 0) {
            if (!buf_append(&buf, &len, &cap, max_bytes, &res->truncated,
                            tmp, (size_t)n)) {
                io_failed = true;
                (void)kill(pid, SIGKILL);
                break;
            }
            continue;
        }
        if (n == 0)
            break;                      /* EOF：子进程已关闭 stdout */
        if (errno == EINTR)
            continue;
        io_failed = true;
        break;
    }

    (void)close(pfd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            break;
    }

    if (!res->timed_out && !io_failed)
        res->status = status;

    if (buf != NULL) {
        if (len + 1 > cap) {
            char *nbuf = realloc(buf, len + 1);
            if (nbuf != NULL)
                buf = nbuf;
        }
        buf[len] = '\0';
        res->out = buf;
        res->out_len = len;
    }

    if (io_failed) {
        res->error = errno;
        return -1;
    }
    return 0;
}

int fmon_child_run(const char *const argv[], size_t max_bytes, int timeout_ms,
                   FmonChildResult *res)
{
    return child_run_impl(argv, max_bytes, timeout_ms, false, res);
}

int fmon_child_run_merged(const char *const argv[], size_t max_bytes,
                          int timeout_ms, FmonChildResult *res)
{
    return child_run_impl(argv, max_bytes, timeout_ms, true, res);
}
