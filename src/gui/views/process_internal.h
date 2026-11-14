/*
 * process_internal.h - 进程页的内部接口
 *
 * 只供 src/gui/views/process_*.c 之间使用；进程页对外的唯一入口仍是
 * process_view.h 中的 gui_process_view_new()。
 *
 * 拆分层次（依赖方向自上而下，禁止反向依赖）：
 *   process_view.c    页面装配、刷新调度、选择、导出
 *   process_list.c    ColumnView、单元格工厂、列排序
 *   process_actions.c 右键菜单、信号 / renice / CPU 绑定
 *   process_windows.c 进程详情（ps）、lsof / sockstat 文本窗口
 *   process_model.c   ProcItem、ps 解析、过滤、排序
 */

#ifndef PROCESS_INTERNAL_H
#define PROCESS_INTERNAL_H

#include <gtk/gtk.h>

#include <sys/types.h>

#include "gui_ctl.h"
#include "gui_util.h"
#include "process_ps.h"
#include "process_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 数据模型
 * ========================================================================= */

/* 列表中的一行。字段对 process_* 模块可见，便于各层直接读取；
 * 进程页以外不得引用。 */
typedef struct {
    GObject parent_instance;

    guint32  pid;
    guint32  uid;
    gchar    state;
    gchar   *user;
    gchar   *comm;
    gdouble  cpu;
    gdouble  mem;
    guint64  rss_kb;
    guint64  swap_kb;
    double   cputime_s;
    guint32  ppid;
    guint32  threads;
    guint16  depth;      /* 树形视图缩进层级 */
    gboolean is_system;
    guint8   category;   /* 0 桌面应用 / 1 用户进程 / 2 系统组件 */
} ProcItem;

typedef struct {
    GObjectClass parent_class;
} ProcItemClass;

GType proc_item_get_type(void);

/* 进程视图：普通用户 / 系统（root）。系统视图只有以 root 运行界面时可用。 */
typedef enum {
    PROC_VIEW_USER = 0,
    PROC_VIEW_SYSTEM = 1
} ProcView;

/* 列序号：排序器用同一个枚举，避免“删一列就错位” */
enum {
    PROC_COL_PID = 0,
    PROC_COL_CATEGORY,
    PROC_COL_USER,
    PROC_COL_STATE,
    PROC_COL_CPU,
    PROC_COL_MEM,
    PROC_COL_TIME,
    PROC_COL_SWAP,
    PROC_COL_PPID,
    PROC_COL_THREADS,
    PROC_COL_COMM,
    PROC_COL_COUNT
};

typedef struct {
    GtkWidget          *root;
    GtkWidget          *scroll;    /* 列表所在的 GtkScrolledWindow */
    GListStore         *store;
    GtkMultiSelection  *selection;
    GtkFilterListModel *filter_model;
    GtkSortListModel   *sort_model;
    GtkCustomFilter    *filter;
    GtkWidget          *search;
    GtkWidget          *btn_user;
    GtkWidget          *btn_system;
    GtkWidget          *hint;
    GtkWidget          *btn_end;
    GtkWidget          *btn_details;

    gchar              *search_text;
    ProcView            view;
    gboolean            is_root;
    uid_t               uid;

    GHashTable         *cpu_map;       /* pid -> top 的 WCPU(%)，可空 */
    GHashTable         *extra_map;     /* pid -> ProcExtra（ppid/rss/swap/time/threads） */
    gboolean            tree_mode;
    guint64             fingerprint;   /* 上次刷新数据的指纹，用于跳过无变化重建 */
    gboolean            busy;
} ProcPage;

/* process_model.c：视图过滤、列排序 */
gboolean proc_filter_func(gpointer item, gpointer user_data);
int      proc_compare(gconstpointer a, gconstpointer b, gpointer data);

/* 多选状态下刷新提示行（右键菜单打开时调用一次，保证计数准确） */
void     proc_page_update_hint(ProcPage *page);

/* ===========================================================================
 * 操作层上下文
 * ========================================================================= */

/* 一次信号 / 优先级操作 */
typedef struct {
    guint32 pid;
    guint32 uid;       /* 目标进程属主，用于“普通用户不能操作 root 进程” */
    gchar  *name;
    int     sig;
    gboolean needs_root;
    gint    nice;
    gboolean is_nice;
} SignalOp;

/* process_actions.c：详情窗口、信号 / renice、右键菜单 */
void      proc_actions_attach_view(GtkWidget *col_view);
SignalOp *proc_actions_signal_op_new(ProcItem *item, int sig,
                                     gboolean needs_confirm);
void      proc_actions_send_signal(gpointer user_data);
void      proc_actions_signal_op_free(SignalOp *op);

/* process_windows.c：详情 / lsof / sockstat 窗口 */
void      proc_windows_show_detail(GtkWindow *parent, ProcItem *item);
void      proc_windows_show_text(GtkWindow *parent, const char *title,
                                 guint32 pid_filter,
                                 const char *const argv[]);

/* process_list.c：模型栈、ColumnView、单元格工厂 */
GtkWidget *proc_list_build(ProcPage *page);
void       proc_list_apply_filter(ProcPage *page);

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_INTERNAL_H */
