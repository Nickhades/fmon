/*
 * gui_window_events.c - 窗口事件：关闭、退出信号、快捷键
 */

#include "gui_window_events.h"
#include "gui_window.h"
#include "gui_window_state.h"
#include "collector_thread.h"

#include "gui_runtime.h"

#include <glib-unix.h>
#include <gtk/gtk.h>

#include <stdlib.h>

extern GtkWindow *g_main_window;

/* 退出：先落盘状态，再直接结束进程。
 *
 * 与 close-request 相同的策略：GTK 4.20 在销毁含多个 GtkColumnView 的窗口时
 * 可能访问已失效对象；如果此刻还有模态确认框打开，gtk_window_close() 会把
 * 销毁流程带进 GTK 内部（曾在 SIGTERM 时产生 core）。
 * SIGTERM/SIGINT/Ctrl+Q 不需要走窗口销毁，直接退出最稳。 */
static void quit_now(void)
{
    gui_window_state_save();
    gui_collector_stop();
    gui_runtime_shutdown();
    exit(0);
}

gboolean gui_window_quit_signal(gpointer user_data)
{
    (void)user_data;

    quit_now();
    return G_SOURCE_CONTINUE;
}

gboolean gui_window_close_request(GtkWindow *window, gpointer data)
{
    (void)data;
    (void)window;

    gui_window_state_save();
    gui_collector_stop();
    gui_runtime_shutdown();

    /* 保存完状态后直接退出进程。
     *
     * 原因：GTK 4.20 在销毁包含多个 GtkColumnView（进程/网络/存储页）的窗口时，
     * 会在 g_datalist_clear -> g_object_unref 里访问已失效的对象（SIGBUS）。
     * 该问题与界面业务代码无关（旧版本同样可复现）；在无法修改 GTK 的前提下，
     * 退出时跳过整棵控件树的销毁是最稳妥的做法：状态已落盘，进程直接结束。 */
    exit(0);

    return FALSE;
}

gboolean gui_window_key_pressed(GtkEventControllerKey *controller,
                               guint keyval, guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)user_data;

    if ((state & GDK_CONTROL_MASK) != 0 && keyval == GDK_KEY_q) {
        quit_now();
        return TRUE;
    }

    if (keyval == GDK_KEY_F5) {
        gui_runtime_snapshot();
        gui_runtime_banner();
        gui_runtime_alert_indicator();
        return TRUE;
    }

    return FALSE;
}
