/*
 * fmon_child.h - 不经过 shell 的子进程执行工具
 *
 * 旧代码大量使用 popen("/bin/sh -c \"...\"")，这带来三类问题：
 *   1. 安全：只要命令串里混入服务名 / 用户名 / 文件名，就可能被 shell 解释，
 *      形成命令注入（例如服务名里带 $(...) 或 ;）。
 *   2. 性能：每次都要 fork 出 sh，再由 sh fork 出目标程序，还多一层管道。
 *   3. 可靠性：sh 的引号嵌套很容易出错，且无法限制超时（zpool iostat 卡住时
 *      整个采集线程会一直阻塞）。
 *
 * 本模块直接 fork + execvp，参数按 argv 传递，语义清晰且不可能被注入。
 */

#ifndef FMON_CHILD_H
#define FMON_CHILD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMON_CHILD_DEFAULT_MAX   (1024u * 1024u)   /* 默认最多收集 1 MiB */
#define FMON_CHILD_DEFAULT_TIMEOUT_MS 4000         /* 默认 4 秒超时 */

typedef struct {
    char  *out;          /* malloc 分配并保证以 '\0' 结尾；无输出时为 NULL */
    size_t out_len;      /* 不含结尾 '\0' */
    int    status;       /* waitpid 原始状态；-1 表示启动失败或超时 */
    int    error;        /* fork/pipe 失败时的 errno；0 表示正常 */
    bool   truncated;    /* 输出超过 max_bytes 被截断 */
    bool   timed_out;    /* 子进程超时被 SIGKILL */
} FmonChildResult;

/*
 * 运行 argv（argv[0] 可以是绝对路径或 PATH 中的名字），捕获 stdout。
 * stderr 被重定向到 /dev/null。
 *
 * argv      : 以 NULL 结尾的参数数组，必须非空
 * max_bytes : 最多保留多少字节输出，0 表示 FMON_CHILD_DEFAULT_MAX
 * timeout_ms: 超时毫秒数，<= 0 表示 FMON_CHILD_DEFAULT_TIMEOUT_MS
 * res       : 输出结构，调用者负责 fmon_child_result_free()
 *
 * 返回 0 表示子进程成功执行完毕（不代表退出码为 0，退出码见 res->status），
 * 返回 -1 表示 fork/pipe 等系统调用失败（errno 在 res->error）。
 */
int  fmon_child_run(const char *const argv[], size_t max_bytes,
                    int timeout_ms, FmonChildResult *res);

/* 同上，但把 stderr 也合并进输出（用于需要显示 pkexec 等错误信息的场景） */
int  fmon_child_run_merged(const char *const argv[], size_t max_bytes,
                           int timeout_ms, FmonChildResult *res);

/* 是否正常退出且退出码为 0 */
bool fmon_child_ok(const FmonChildResult *res);

/* 释放 res 内部资源，可重复调用 */
void fmon_child_result_free(FmonChildResult *res);

#ifdef __cplusplus
}
#endif

#endif /* FMON_CHILD_H */
