/*
 * fmon_shm.c - 共享内存的创建 / 校验 / 加锁 / 快照实现
 *
 * 这个文件同时被 fmon-daemon（写者）和 fmon-gui（读者）链接，
 * 保证两端对“什么算合法数据”的理解完全一致。
 */

#include "fmon_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* 守护进程内的写者互斥锁。只有守护进程会调用 writer 相关函数。 */
static pthread_mutex_t g_wr_lock = PTHREAD_MUTEX_INITIALIZER;
static FmonShmData    *g_wr_shm  = NULL;

static void set_error(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_error(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0)
        return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* 共享内存对象权限：
 *   - 0644：任何本地用户都可以读取（界面不需要 root），
 *           但只有 root 能写，防止其他用户伪造监控数据。
 *   - 旧版本用 0666，任何本地用户都能改数据，属于完整性风险。 */
static void tune_permissions(int fd)
{
    if (fd < 0)
        return;
    (void)fchmod(fd, 0644);
}

/* ---------------------------------------------------------------------------
 * 单实例保护
 *
 * 不能对 POSIX 共享内存对象使用 flock()：FreeBSD 的 shm 后端（tmpfs）
 * 会返回 EOPNOTSUPP。因此采用两层保护：
 *   1. /var/run 下的锁文件 + flock（普通文件系统支持 flock）；
 *   2. 共享内存头部里的 daemon_pid + kill(pid, 0) 存活探测
 *      （即使锁文件不可用，也能识别出还在运行的旧实例）。
 * ------------------------------------------------------------------------- */
#define FMON_LOCKFILE_RUN  "/var/run/fmon-daemon.lock"
#define FMON_LOCKFILE_TMP  "/tmp/fmon-daemon.lock"

static int g_lock_fd = -1;

static int acquire_lockfile(bool *flock_works)
{
    static const char *const paths[] = { FMON_LOCKFILE_RUN, FMON_LOCKFILE_TMP };

    *flock_works = false;

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0)
            continue;

        int rc = flock(fd, LOCK_EX | LOCK_NB);
        if (rc == 0) {
            char buf[32];
            int  n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
            if (ftruncate(fd, 0) == 0)
                (void)write(fd, buf, (size_t)n);
            *flock_works = true;
            return fd;
        }

        if (errno == EOPNOTSUPP || errno == ENOLCK || errno == EINVAL) {
            /* 该文件系统不支持 flock，交给 pid 存活探测兜底 */
            return fd;
        }

        close(fd);
        return -2;   /* 已被其他进程持有 */
    }

    return -1;       /* 没有可用的锁文件路径 */
}

/* 读取共享内存头部，判断是否已有存活的守护进程 */
static bool daemon_alive_in_shm(int fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0)
        return false;
    if ((uint64_t)st.st_size < sizeof(FmonShmData))
        return false;

    void *p = mmap(NULL, sizeof(FmonShmData), PROT_READ, MAP_SHARED, fd, 0);

    /* UI 每次快照都顺序拷贝整个结构：提示内核顺序访问，减少预读错判 */
    if (p != MAP_FAILED)
        (void)madvise(p, sizeof(FmonShmData), MADV_SEQUENTIAL);
    if (p == MAP_FAILED)
        return false;

    const FmonShmData *shm = p;
    bool alive = false;

    if (shm->magic == FMON_SHM_MAGIC && shm->abi_version == FMON_ABI_VERSION &&
        (shm->status == (int32_t)FMON_STATUS_RUNNING ||
         shm->status == (int32_t)FMON_STATUS_INIT)) {
        pid_t pid = (pid_t)shm->daemon_pid;
        if (pid > 0 && pid != getpid()) {
            if (kill(pid, 0) == 0 || errno == EPERM)
                alive = true;
        }
    }

    munmap(p, sizeof(FmonShmData));
    return alive;
}

static FmonShmData *shm_create_impl(const char *name, int *out_fd, bool force,
                                    bool check_running, char *err,
                                    size_t err_len)
{
    if (out_fd != NULL)
        *out_fd = -1;

    /* O_CLOEXEC：避免子进程继承共享内存 fd */
    int fd = shm_open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_error(err, err_len, "shm_open(%s) 失败: %s", name,
                  strerror(errno));
        return NULL;
    }

    /* 第一层：锁文件（flock 不受支持时返回 fd 但 *flock_works == false） */
    bool flock_works = false;
    int  lock_fd     = acquire_lockfile(&flock_works);

    if (check_running && lock_fd == -2 && !force) {
        set_error(err, err_len,
                  "已有另一个 fmon-daemon 在运行（锁文件被占用）");
        close(fd);
        return NULL;
    }
    if (lock_fd >= 0)
        g_lock_fd = lock_fd;

    /* 第二层：共享内存里的 pid 存活探测（覆盖 flock 不可用的情况） */
    if (check_running && !force && daemon_alive_in_shm(fd)) {
        set_error(err, err_len,
                  "已有另一个 fmon-daemon 在运行（共享内存中的 pid 仍然存活）；"
                  "如确认需要强制接管，请使用 -f 参数");
        close(fd);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_error(err, err_len, "fstat 失败: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    if (st.st_size != (off_t)sizeof(FmonShmData)) {
        /* 首次创建、旧版本残留或结构体变更：按当前布局重建。
         * ftruncate 会清空内容，这是有意的——旧布局的数据不可信。 */
        if (ftruncate(fd, (off_t)sizeof(FmonShmData)) != 0) {
            set_error(err, err_len, "ftruncate 失败: %s", strerror(errno));
            close(fd);
            return NULL;
        }
    }

    void *p = mmap(NULL, sizeof(FmonShmData), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        set_error(err, err_len, "mmap 失败: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    FmonShmData *shm = p;
    memset(shm, 0, sizeof(*shm));

    /* 先写头部静态部分，seq 保持偶数（0），此时没有读者会认为数据有效 */
    shm->magic       = FMON_SHM_MAGIC;
    shm->abi_version = FMON_ABI_VERSION;
    shm->struct_size = (uint32_t)sizeof(FmonShmData);
    shm->daemon_pid  = (int32_t)getpid();
    shm->status      = (int32_t)FMON_STATUS_INIT;

    tune_permissions(fd);

    if (out_fd != NULL)
        *out_fd = fd;

    return shm;
}

static FmonShmData *shm_attach_impl(const char *name, bool writable,
                                    char *err, size_t err_len)
{
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;

    int fd = shm_open(name, flags, 0);
    if (fd < 0) {
        if (errno == ENOENT)
            set_error(err, err_len, "未找到共享内存 %s（守护进程未启动？）",
                      name);
        else if (errno == EACCES)
            set_error(err, err_len,
                      "没有权限读取 %s（请检查 /var/run/shm 权限或把用户加入 operator 组）",
                      name);
        else
            set_error(err, err_len, "shm_open 失败: %s", strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_error(err, err_len, "fstat 失败: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    /* 关键安全修复：旧版本不看文件大小直接 mmap，如果守护进程刚创建、
     * 或者两端结构体大小不同，界面一读就越界并触发 SIGBUS。
     * 这里先比对大小，再映射固定长度。 */
    if ((uint64_t)st.st_size < (uint64_t)sizeof(FmonShmData)) {
        set_error(err, err_len,
                  "共享内存大小不匹配（实际 %lld 字节，需要 %zu 字节）："
                  "守护进程与界面的版本不一致，请重新编译并重启两者",
                  (long long)st.st_size, sizeof(FmonShmData));
        close(fd);
        return NULL;
    }

    void *p = mmap(NULL, sizeof(FmonShmData),
                   writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   MAP_SHARED, fd, 0);
    close(fd); /* 映射建立后 fd 即可关闭 */

    if (p == MAP_FAILED) {
        set_error(err, err_len, "mmap 失败: %s", strerror(errno));
        return NULL;
    }

    FmonShmData *shm = p;

    if (shm->magic != FMON_SHM_MAGIC || shm->abi_version != FMON_ABI_VERSION ||
        shm->struct_size != (uint32_t)sizeof(FmonShmData)) {
        set_error(err, err_len,
                  "共享内存 ABI 不兼容（magic=0x%08x abi=%u size=%u，期望 "
                  "0x%08x/%u/%zu）：请同时重启 daemon 与 gui",
                  shm->magic, shm->abi_version, shm->struct_size,
                  FMON_SHM_MAGIC, FMON_ABI_VERSION, sizeof(FmonShmData));
        munmap(shm, sizeof(FmonShmData));
        return NULL;
    }

    return shm;
}

/* ---- 公开入口：固定名字（守护进程/GUI 使用） + 自定义名字（单元测试） ---- */

FmonShmData *fmon_shm_create(int *out_fd, bool force, char *err,
                             size_t err_len)
{
    return shm_create_impl(FMON_SHM_NAME, out_fd, force, true, err, err_len);
}

FmonShmData *fmon_shm_create_for_test(const char *name, int *out_fd, char *err,
                                      size_t err_len)
{
    return shm_create_impl(name, out_fd, true, false, err, err_len);
}

FmonShmData *fmon_shm_attach(bool writable, char *err, size_t err_len)
{
    return shm_attach_impl(FMON_SHM_NAME, writable, err, err_len);
}

FmonShmData *fmon_shm_attach_for_test(const char *name, bool writable,
                                      char *err, size_t err_len)
{
    return shm_attach_impl(name, writable, err, err_len);
}

void fmon_shm_detach(FmonShmData *shm)
{
    if (shm == NULL)
        return;
    munmap(shm, sizeof(FmonShmData));
}

void fmon_shm_writer_init(FmonShmData *shm)
{
    g_wr_shm = shm;
    if (shm != NULL)
        atomic_store_explicit(&shm->seq, 0, memory_order_relaxed);
}

void fmon_shm_wr_begin(void)
{
    pthread_mutex_lock(&g_wr_lock);

    if (g_wr_shm == NULL)
        return;

    /* 进入写事务：先把 seq 标成奇数，release fence 保证“奇数”先于
     * 后续数据写入可见，读者不会把半更新数据当成有效快照。 */
    uint32_t s = atomic_load_explicit(&g_wr_shm->seq, memory_order_relaxed);

    atomic_store_explicit(&g_wr_shm->seq, s | 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

void fmon_shm_wr_end(void)
{
    if (g_wr_shm != NULL) {
        /* 数据写完再放 release fence，最后把 seq 置回偶数（提交） */
        atomic_thread_fence(memory_order_release);

        uint32_t s = atomic_load_explicit(&g_wr_shm->seq,
                                          memory_order_relaxed);

        atomic_store_explicit(&g_wr_shm->seq, s + 1u, memory_order_relaxed);
    }

    pthread_mutex_unlock(&g_wr_lock);
}

/* 逐字节的 volatile 拷贝：告诉编译器“这块内存可能被别的进程同时改写”，
 * 不允许它把循环优化成一次性 memcpy 或生成非原子的宽读写。 */
static void copy_volatile(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const volatile unsigned char *s = src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
}

int fmon_shm_snapshot(const FmonShmData *src, FmonShmData *dst)
{
    if (src == NULL || dst == NULL)
        return -1;

    for (int attempt = 0; attempt < 64; attempt++) {
        uint32_t before = atomic_load_explicit(&src->seq,
                                               memory_order_acquire);

        if ((before & 1u) != 0u) {
            /* 写事务进行中，稍后重试 */
            sched_yield();
            continue;
        }

        copy_volatile(dst, src, sizeof(*dst));

        atomic_thread_fence(memory_order_acquire);
        uint32_t after = atomic_load_explicit(&src->seq,
                                              memory_order_acquire);

        if (before == after)
            return 0;
    }

    return -1;
}


