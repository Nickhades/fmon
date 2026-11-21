/*
 * gui_language_switch.c - 语言切换与进程重启
 */

#include "gui_language_switch.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "gui_util.h"
#include "gui_window.h"
#include "gui_window_state.h"
#include "language.h"
#include "theme.h"

extern GtkWindow *g_main_window;

static gchar *s_executable_path;

void fmon_gui_set_executable_path(const char *argv0)
{
    g_free(s_executable_path);

    if (argv0 == NULL) {
        s_executable_path = NULL;
        return;
    }

    if (strchr(argv0, G_DIR_SEPARATOR) != NULL)
        s_executable_path = g_canonicalize_filename(argv0, NULL);
    else
        s_executable_path = g_find_program_in_path(argv0);

    if (s_executable_path == NULL)
        s_executable_path = g_strdup(argv0);
}

static gboolean s_lang_syncing = FALSE;

void gui_language_set_syncing(gboolean syncing)
{
    s_lang_syncing = syncing;
}

void gui_language_toggled(GtkToggleButton *btn, gpointer data)
{
    if (s_lang_syncing || !gtk_toggle_button_get_active(btn))
        return;

    FmonLanguage target = (FmonLanguage)GPOINTER_TO_INT(data);

    if (target == fmon_language_get())
        return;

    fmon_language_set(target);
    fmon_gui_restart_for_language();
}

void fmon_gui_restart_for_language(void)
{
    gchar *exe = (s_executable_path != NULL)
                       ? g_strdup(s_executable_path)
                       : g_find_program_in_path("fmon-gui");

    if (exe == NULL) {
        fmon_gui_show_error(g_main_window, _("无法切换语言"),
                            _("找不到 fmon-gui 可执行文件。"));
        return;
    }

    const gchar *page = gui_window_current_page();
    const gchar *theme = fmon_theme_is_dark() ? "dark" : "light";
    gchar *page_arg = g_strdup((page != NULL) ? page : "cpu");
    gchar *exe_arg = g_strdup(exe);

    /* exec 替换进程，旧 GTK 控件与旧 gettext 状态全部丢弃，
     * 新进程按已保存的语言重新构建整个界面。 */
    gchar *argv[] = { exe_arg, (gchar *)"--page", page_arg,
                      (gchar *)"--theme", (gchar *)theme, NULL };

    gui_window_state_save();
    g_free(exe);
    execv(exe_arg, argv);

    int err = errno;
    gchar *msg = g_strdup_printf(_("无法重启 fmon-gui：%s"),
                                 g_strerror(err));

    fmon_gui_show_error(g_main_window, _("语言切换失败"), msg);
    g_free(msg);
    g_free(page_arg);
    g_free(exe_arg);
}
