/*
 * theme.c - 浅色 / 深色主题管理实现
 */

#include "theme.h"

#include <glib/gstdio.h>
#include <string.h>

static FmonThemeMode g_mode      = FMON_THEME_AUTO;
static gboolean      g_dark      = TRUE;
static gboolean      g_applying  = FALSE;
static void        (*g_observer)(void) = NULL;

/* ------------------------------------------------------------------------- */
/* 持久化                                                                    */
/* ------------------------------------------------------------------------- */

static gchar *theme_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fmon", "theme", NULL);
}

static void theme_save(void)
{
    gchar *path = theme_config_path();
    gchar *dir  = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);

    const char *txt = (g_mode == FMON_THEME_LIGHT) ? "light"
                      : (g_mode == FMON_THEME_DARK) ? "dark"
                                                    : "auto";

    g_file_set_contents(path, txt, -1, NULL);

    g_free(dir);
    g_free(path);
}

static void theme_load(void)
{
    gchar *path = theme_config_path();
    gchar *data = NULL;

    if (g_file_get_contents(path, &data, NULL, NULL) && data != NULL) {
        g_strstrip(data);

        if (g_strcmp0(data, "light") == 0)
            g_mode = FMON_THEME_LIGHT;
        else if (g_strcmp0(data, "dark") == 0)
            g_mode = FMON_THEME_DARK;
        else
            g_mode = FMON_THEME_AUTO;
    }

    g_free(data);
    g_free(path);
}

/* ------------------------------------------------------------------------- */
/* 系统颜色方案                                                              */
/* ------------------------------------------------------------------------- */

static gboolean system_prefers_dark(void)
{
    GtkSettings *settings = gtk_settings_get_default();

    if (settings == NULL)
        return TRUE;

    GParamSpec *ps = g_object_class_find_property(
        G_OBJECT_GET_CLASS(settings), "gtk-interface-color-scheme");

    if (ps != NULL) {
        gint scheme = GTK_INTERFACE_COLOR_SCHEME_DEFAULT;

        g_object_get(settings, "gtk-interface-color-scheme", &scheme, NULL);

        if (scheme == GTK_INTERFACE_COLOR_SCHEME_DARK)
            return TRUE;
        if (scheme == GTK_INTERFACE_COLOR_SCHEME_LIGHT)
            return FALSE;
    }

    gboolean prefer_dark = FALSE;

    g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark,
                 NULL);

    return prefer_dark;
}

/* ------------------------------------------------------------------------- */
/* 应用                                                                      */
/* ------------------------------------------------------------------------- */

static void theme_apply(void)
{
    GtkSettings *settings = gtk_settings_get_default();

    if (settings == NULL)
        return;

    g_applying = TRUE;

    if (g_mode == FMON_THEME_AUTO)
        g_dark = system_prefers_dark();
    else
        g_dark = (g_mode == FMON_THEME_DARK);

    g_object_set(settings, "gtk-application-prefer-dark-theme", g_dark, NULL);

    GParamSpec *ps = g_object_class_find_property(
        G_OBJECT_GET_CLASS(settings), "gtk-interface-color-scheme");

    if (ps != NULL) {
        gint scheme;

        if (g_mode == FMON_THEME_AUTO)
            scheme = GTK_INTERFACE_COLOR_SCHEME_DEFAULT;
        else if (g_dark)
            scheme = GTK_INTERFACE_COLOR_SCHEME_DARK;
        else
            scheme = GTK_INTERFACE_COLOR_SCHEME_LIGHT;

        g_object_set(settings, "gtk-interface-color-scheme", scheme, NULL);
    }

    g_applying = FALSE;

    if (g_observer != NULL)
        g_observer();
}

static void on_settings_changed(GObject *obj, GParamSpec *ps, gpointer data)
{
    (void)obj;
    (void)ps;
    (void)data;

    if (g_applying || g_mode != FMON_THEME_AUTO)
        return;

    gboolean was = g_dark;
    g_dark = system_prefers_dark();

    if (was != g_dark && g_observer != NULL)
        g_observer();
}

void fmon_theme_init(void)
{
    theme_load();
    theme_apply();

    GtkSettings *settings = gtk_settings_get_default();

    if (settings != NULL) {
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
                         G_CALLBACK(on_settings_changed), NULL);

        if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings),
                                         "gtk-interface-color-scheme") != NULL)
            g_signal_connect(settings,
                             "notify::gtk-interface-color-scheme",
                             G_CALLBACK(on_settings_changed), NULL);
    }
}

void fmon_theme_set(FmonThemeMode mode)
{
    if (mode != FMON_THEME_AUTO && mode != FMON_THEME_LIGHT &&
        mode != FMON_THEME_DARK)
        return;

    g_mode = mode;
    theme_save();
    theme_apply();
}

FmonThemeMode fmon_theme_mode(void)
{
    return g_mode;
}

gboolean fmon_theme_is_dark(void)
{
    return g_dark;
}

void fmon_theme_set_observer(void (*cb)(void))
{
    g_observer = cb;
}

/* ------------------------------------------------------------------------- */
/* Cairo 配色                                                                */
/* ------------------------------------------------------------------------- */

void fmon_theme_panel_colors(double top[4], double bottom[4])
{
    if (g_dark) {
        top[0] = 0.088; top[1] = 0.094; top[2] = 0.112; top[3] = 0.96;
        bottom[0] = 0.040; bottom[1] = 0.044; bottom[2] = 0.056;
        bottom[3] = 0.96;
    } else {
        top[0] = 0.965; top[1] = 0.968; top[2] = 0.975; top[3] = 0.98;
        bottom[0] = 0.900; bottom[1] = 0.905; bottom[2] = 0.915;
        bottom[3] = 0.98;
    }
}

void fmon_theme_texture_color(double rgba[4])
{
    if (g_dark) {
        rgba[0] = 1.0; rgba[1] = 1.0; rgba[2] = 1.0; rgba[3] = 0.030;
    } else {
        rgba[0] = 0.0; rgba[1] = 0.0; rgba[2] = 0.0; rgba[3] = 0.035;
    }
}

void fmon_theme_grid_color(double rgba[4], gboolean separator)
{
    if (g_dark) {
        rgba[0] = 1.0; rgba[1] = 1.0; rgba[2] = 1.0;
        rgba[3] = separator ? 0.20 : 0.10;
    } else {
        rgba[0] = 0.0; rgba[1] = 0.0; rgba[2] = 0.0;
        rgba[3] = separator ? 0.12 : 0.06;
    }
}

void fmon_theme_axis_color(double rgba[4])
{
    if (g_dark) {
        rgba[0] = 0.62; rgba[1] = 0.62; rgba[2] = 0.68; rgba[3] = 0.55;
    } else {
        rgba[0] = 0.28; rgba[1] = 0.30; rgba[2] = 0.36; rgba[3] = 0.85;
    }
}

void fmon_theme_hover_line(double rgba[4])
{
    if (g_dark) {
        rgba[0] = 0.90; rgba[1] = 0.90; rgba[2] = 0.95; rgba[3] = 0.35;
    } else {
        rgba[0] = 0.0; rgba[1] = 0.0; rgba[2] = 0.0; rgba[3] = 0.35;
    }
}

void fmon_theme_hover_bg(double rgba[4])
{
    if (g_dark) {
        rgba[0] = 0.0; rgba[1] = 0.0; rgba[2] = 0.0; rgba[3] = 0.72;
    } else {
        rgba[0] = 1.0; rgba[1] = 1.0; rgba[2] = 1.0; rgba[3] = 0.95;
    }
}
