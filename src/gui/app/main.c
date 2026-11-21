/*
 * main.c - 进程入口与命令行解析
 *
 * 窗口、页面与刷新循环在 gui_window.c；导航在 gui_nav.c。
 * 这里只负责：字体/locale 初始化、注册命令行选项、启动 GtkApplication。
 */

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <execinfo.h>
#include <locale.h>
#include <malloc_np.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "font_setup.h"
#include "gui_window.h"
#include "language.h"

/* 崩溃诊断：设置 FMON_TRACE_SIGNAL=1 时，SIGBUS/SIGSEGV/SIGABRT 打印
 * 调用栈后退出（用于定位 GTK 内部崩溃）。 */
static void fmon_trace_signal(int sig)
{
    void *bt[64];
    int   n = backtrace(bt, 64);

    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(128 + sig);
}

static void fmon_trace_init(void)
{
    if (getenv("FMON_TRACE_SIGNAL") == NULL)
        return;

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fmon_trace_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* 及时归还空闲页：jemalloc 默认延迟 decay 会让 RSS 缓慢上涨。
 * 已验证 1 秒刷新下 RSS 会稳定在 ~220MB；默认开启，
 * 如需退回旧行为可设 FMON_NO_MALLOC_DECAY=1。 */
static void fmon_malloc_tune(void)
{
    if (getenv("FMON_NO_MALLOC_DECAY") != NULL)
        return;

    size_t zero = 0;

    mallctl("arenas.dirty_decay_ms", NULL, NULL, &zero, sizeof(zero));
    mallctl("arenas.muzzy_decay_ms", NULL, NULL, &zero, sizeof(zero));
}

int main(int argc, char **argv)
{
    fmon_trace_init();
    fmon_malloc_tune();
    fmon_gui_set_executable_path(argv[0]);
    /* 字体渲染设置必须在 GTK 初始化之前（强制 CPU 渲染 + 固定抗锯齿） */
    fmon_font_setup();

    /* 语言必须在 setlocale 前确定；英文语言包缺失时 gettext 会回退 msgid。
     * FMON_LOCALEDIR 仅用于未安装时的本地测试。 */
    fmon_language_init();
    setlocale(LC_ALL, "");

    const char *locale_dir = g_getenv("FMON_LOCALEDIR");
    bindtextdomain(GETTEXT_PACKAGE,
                   (locale_dir != NULL && *locale_dir != '\0')
                       ? locale_dir : LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    /* LC_NUMERIC 必须保持 "C"，否则 printf("%f") 会输出逗号小数点 */
    setlocale(LC_NUMERIC, "C");

    /* 部分输入法在 GTK4 下需要简单上下文 */
    g_setenv("GTK_IM_MODULE", "gtk-im-context-simple", TRUE);

    /* 桌面入口/任务栏的 WM_CLASS 与 application id 保持一致。 */
    g_set_prgname("org.freebsd.fmon");
    g_set_application_name(_("FreeBSD 监控中心"));

    GtkApplication *app = gtk_application_new("org.freebsd.fmon",
                                              G_APPLICATION_DEFAULT_FLAGS);

    g_application_add_main_option(
        G_APPLICATION(app), "page", 'p', G_OPTION_FLAG_NONE,
        G_OPTION_ARG_STRING,
        "启动时显示的页面（如 cpu/mem/disk/gpu1/proc/...）", "PAGE");
    g_application_add_main_option(
        G_APPLICATION(app), "gpu", 'g', G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
        "启动时打开的显卡序号（从 0 开始）", "N");
    g_application_add_main_option(
        G_APPLICATION(app), "theme", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_STRING, "起始主题：auto | light | dark", "MODE");
    g_application_add_main_option(
        G_APPLICATION(app), "version", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_NONE, "显示版本信息并退出", NULL);

    g_signal_connect(app, "handle-local-options",
                     G_CALLBACK(gui_window_handle_options), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(gui_window_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}
