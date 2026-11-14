/*
 * service_internal.h - 服务页的内部接口
 *
 * 只供 src/gui/views/service_*.c 使用；对外入口仍是 service_view.h 中的
 * gui_service_view_new()。
 *
 * 层次（依赖方向自上而下）：
 *   service_view.c    页面装配、刷新调度
 *   service_list.c    行构建、列表重建、过滤
 *   service_actions.c 启动 / 停止 / 自启操作、详情窗口
 *   service_model.c   rc.conf 解析、脚本扫描
 */

#ifndef SERVICE_INTERNAL_H
#define SERVICE_INTERNAL_H

#include <gtk/gtk.h>

#include "gui_ctl.h"
#include "gui_util.h"
#include "service_rc.h"
#include "service_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    GtkWidget *notebook;      /* 持有 page 的生命周期 */
    GtkWidget *list_running;
    GtkWidget *list_stopped;
    GtkWidget *tab_running;
    GtkWidget *tab_stopped;
    GtkWidget *search;
    GtkWidget *btn_internal;

    GHashTable *running;      /* name -> (gpointer)1 */
    GHashTable *selected;     /* 批量操作勾选的服务名 */
    GHashTable *vars;         /* rc.conf 变量 -> 值 */
    GPtrArray  *names;        /* gchar* */

    gboolean hide_internal;
    gboolean loading;
} ServicePage;

/* 一次服务操作（service start/stop 或 sysrc 开关） */
typedef struct {
    GtkWidget *notebook;   /* 持有引用，保证回调期间 page / 控件有效 */
    gchar     *name;
    gchar     *action;     /* service 的动作，或 NULL 表示 sysrc */
    gboolean   enable;     /* sysrc 的值 */
} SvcOp;

/* 行内按钮上的轻量数据 */
typedef struct {
    gchar *name;
    gchar *action;    /* start / stop，可为 NULL */
    gchar *sysrc;     /* "YES" / "NO"，可为 NULL */
} RowAction;

/* 服务详情窗口 */
typedef struct {
    GtkWindow *window;   /* 弱指针，窗口销毁后自动变 NULL */
    gchar     *name;
} DetailCtx;

/* 列表行的附加数据 */
typedef struct {
    gchar   *name;
    gboolean internal;
} RowInfo;

/* service_model.c：rc.d 脚本扫描 */
GPtrArray  *svc_scan_scripts(void);
gboolean    svc_is_third_party(const char *name);
gboolean    svc_supports_reload(const char *name);

/* service_actions.c：启停 / 自启操作、详情窗口 */
void svc_actions_op_free(SvcOp *op);
void svc_actions_row_action_free(gpointer data);
void svc_actions_start_stop(GtkButton *btn, gpointer data);
void svc_actions_restart(GtkButton *btn, gpointer data);
void svc_actions_reload(GtkButton *btn, gpointer data);
void svc_actions_status(GtkButton *btn, gpointer data);
void svc_actions_logs(GtkButton *btn, gpointer data);
void svc_actions_batch_start(GtkButton *btn, gpointer data);
void svc_actions_batch_stop(GtkButton *btn, gpointer data);
void svc_actions_enable(GtkButton *btn, gpointer data);
void svc_actions_show_detail(GtkButton *btn, gpointer data);

/* service_list.c：行构建、列表重建、过滤 */
void svc_list_rebuild(ServicePage *page);
void svc_list_apply_filter(ServicePage *page);
void svc_list_setup_filters(ServicePage *page);

/* service_view.c：操作完成后刷新页面 */
void svc_page_refresh(GtkWidget *notebook);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_INTERNAL_H */
