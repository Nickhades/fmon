/*
 * process_ps.h - ps 输出解析（纯 GLib，不依赖 GTK）
 *
 * 单独成文件是为了让解析器可以被 tests/test_process_ps.c 直接编译测试：
 * 进程页的其它部分都会拉起 GTK，而这里只做字符串到 PsRow 的转换。
 */

#ifndef PROCESS_PS_H
#define PROCESS_PS_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ps 的一行（先解析成轻量结构，指纹相同就不创建 GObject） */
typedef struct {
    guint32  pid;
    guint32  uid;
    gchar    state;
    gdouble  cpu;
    gdouble  mem;
    guint64  rss_kb;
    gchar   *user;
    gchar   *comm;
    gboolean is_system;
} PsRow;

/* 解析 "pid uid user pcpu pmem rss stat command" 格式的 ps 输出。
 * 表头行、重复 pid、本程序自己拉起的 ps 辅助进程会被跳过；
 * 返回数据指纹（FNV-1a），用于判断数据是否变化。 */
guint64 proc_parse_ps_rows(const gchar *output, GPtrArray *rows);

/* 进程分类：0 = 桌面应用，1 = 其他用户进程，2 = 系统/桌面组件。
 * 纯函数（仅依赖 command/uid/is_system），便于单测。 */
guint8      proc_classify_category(const gchar *command, guint32 uid,
                                   gboolean is_system);
const gchar *proc_category_name(guint8 category);

/* GPtrArray 的 free func，释放一条 PsRow */
void proc_ps_row_free(gpointer data);

/* 内核视图里的 CPU 占用（与 top 的 WCPU 同源），返回 pid -> % 的哈希表
 * （g_int_hash / g_int_equal，值是指向 double 的指针，调用者负责 destroy；
 * 失败返回 NULL，调用方回退到 ps 的 pcpu）。ps 的 pcpu 只打印一位小数，
 * 空闲进程全被抹成 0.0；这里用 ki_pctcpu 原值，可显示两位小数。 */
GHashTable *proc_ps_read_wcpu(void);

/* pid -> ProcExtra：ppid / rss / 上次换出前 RSS / CPU 时间 / 线程数
 * 一次 kern.proc.all 读取，避免为这些列再 fork ps。 */
typedef struct {
    guint32 ppid;
    guint64 rss_kb;
    guint64 swap_kb;
    double  cputime_s;
    guint32 threads;
} ProcExtra;

GHashTable *proc_ps_read_extras(void);

/* 用 libprocstat 读取全部进程，生成与 ps 相同列格式的文本；失败返回 NULL。 */
gchar *proc_read_libprocstat_text(void);

/* 从 kern.proc.all 的原始缓冲区聚合出 pid -> %（按线程求和），
 * 单独导出便于单元测试。len 不是 sizeof(struct kinfo_proc) 的倍数时
 * 忽略尾部残片。 */
GHashTable *proc_ps_parse_kproc(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_PS_H */
