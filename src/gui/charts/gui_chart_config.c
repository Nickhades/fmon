/*
 * gui_chart_config.c - 图表自动量程配置
 */

#include "gui_chart.h"

#include <glib/gstdio.h>

#include <string.h>

static gboolean g_chart_auto_range;

static gchar *chart_settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fmon",
                            "chart_auto_range", NULL);
}

void gui_chart_settings_init(void)
{
    gchar *path = chart_settings_path();
    gchar *data = NULL;

    if (g_file_get_contents(path, &data, NULL, NULL) && data != NULL) {
        g_strstrip(data);
        g_chart_auto_range = (strcmp(data, "1") == 0);
    }

    g_free(data);
    g_free(path);
}

gboolean gui_chart_auto_range(void)
{
    return g_chart_auto_range;
}

void gui_chart_set_auto_range(gboolean enabled)
{
    g_chart_auto_range = enabled ? TRUE : FALSE;

    gchar *path = chart_settings_path();
    gchar *dir  = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);
    (void)g_file_set_contents(path, g_chart_auto_range ? "1\n" : "0\n", -1,
                              NULL);

    g_free(dir);
    g_free(path);
}
