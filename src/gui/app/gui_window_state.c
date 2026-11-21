/*
 * gui_window_state.c - 主窗口状态（大小、居中）
 *
 * 只负责窗口几何，不依赖页面/导航/刷新模块。
 */

#include "gui_window_state.h"

#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>

#include <glib/gstdio.h>

#define WINDOW_MIN_W 1200
#define WINDOW_MIN_H 760
#define WINDOW_DEFAULT_W 1500
#define WINDOW_DEFAULT_H 920

static GtkWindow *s_window;
static int        s_width = WINDOW_DEFAULT_W;
static int        s_height = WINDOW_DEFAULT_H;
static int        s_last_normal_w;
static int        s_last_normal_h;

static gchar *state_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fmon", "window.ini", NULL);
}

static void clamp_to_monitor(void)
{
    GdkDisplay *display = gdk_display_get_default();

    if (display == NULL)
        return;

    GListModel *mons = gdk_display_get_monitors(display);
    GdkMonitor *mon = g_list_model_get_item(mons, 0);

    if (mon != NULL) {
        GdkRectangle geo;

        gdk_monitor_get_geometry(mon, &geo);

        if (s_width > geo.width)
            s_width = geo.width;
        if (s_height > geo.height)
            s_height = geo.height;

        g_object_unref(mon);
    }
}

void gui_window_state_init(void)
{
    gchar    *path = state_path();
    GKeyFile *kf   = g_key_file_new();
    GError   *err  = NULL;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        gint w = g_key_file_get_integer(kf, "window", "width", NULL);
        gint h = g_key_file_get_integer(kf, "window", "height", NULL);

        if (w >= WINDOW_MIN_W && h >= WINDOW_MIN_H) {
            s_width = w;
            s_height = h;
        }
    }

    g_clear_error(&err);
    g_key_file_free(kf);
    g_free(path);

    if (s_width < WINDOW_MIN_W)
        s_width = WINDOW_MIN_W;
    if (s_height < WINDOW_MIN_H)
        s_height = WINDOW_MIN_H;

    clamp_to_monitor();

    s_last_normal_w = s_width;
    s_last_normal_h = s_height;
}

static void on_size_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    (void)data;

    GtkWindow *win = GTK_WINDOW(obj);

    if (!gtk_widget_get_mapped(GTK_WIDGET(win)) ||
        gtk_window_is_maximized(win))
        return;

    int w = gtk_widget_get_width(GTK_WIDGET(win));
    int h = gtk_widget_get_height(GTK_WIDGET(win));

    if (w >= WINDOW_MIN_W && h >= WINDOW_MIN_H) {
        s_last_normal_w = w;
        s_last_normal_h = h;
    }
}

void gui_window_state_attach(GtkWindow *window)
{
    s_window = window;

    gtk_window_set_default_size(window, s_width, s_height);
    gtk_widget_set_size_request(GTK_WIDGET(window), WINDOW_MIN_W,
                                WINDOW_MIN_H);

    g_signal_connect(window, "notify::width", G_CALLBACK(on_size_changed),
                     NULL);
    g_signal_connect(window, "notify::height", G_CALLBACK(on_size_changed),
                     NULL);
}

void gui_window_state_save(void)
{
    if (s_window == NULL)
        return;

    int w;
    int h;

    if (gtk_window_is_maximized(s_window) &&
        s_last_normal_w >= WINDOW_MIN_W &&
        s_last_normal_h >= WINDOW_MIN_H) {
        w = s_last_normal_w;
        h = s_last_normal_h;
    } else {
        w = gtk_widget_get_width(GTK_WIDGET(s_window));
        h = gtk_widget_get_height(GTK_WIDGET(s_window));
    }

    if (w < WINDOW_MIN_W || h < WINDOW_MIN_H)
        return;

    gchar    *path = state_path();
    gchar    *dir  = g_path_get_dirname(path);
    GKeyFile *kf   = g_key_file_new();

    g_mkdir_with_parents(dir, 0700);
    g_key_file_set_integer(kf, "window", "width", w);
    g_key_file_set_integer(kf, "window", "height", h);
    (void)g_key_file_save_to_file(kf, path, NULL);

    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static void center_now(void)
{
    if (s_window == NULL || !gtk_widget_get_mapped(GTK_WIDGET(s_window)))
        return;

    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(s_window));

    if (display == NULL || !GDK_IS_X11_DISPLAY(display))
        return;

    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(s_window));

    if (surface == NULL || !GDK_IS_X11_SURFACE(surface))
        return;

    GListModel *mons = gdk_display_get_monitors(display);
    GdkMonitor *mon = g_list_model_get_item(mons, 0);

    if (mon == NULL)
        return;

    GdkRectangle geo;
    gdk_monitor_get_geometry(mon, &geo);
    g_object_unref(mon);

    int w = gtk_widget_get_width(GTK_WIDGET(s_window));
    int h = gtk_widget_get_height(GTK_WIDGET(s_window));
    int x = geo.x + (geo.width - w) / 2;
    int y = geo.y + (geo.height - h) / 2;

    if (x < geo.x)
        x = geo.x;
    if (y < geo.y)
        y = geo.y;

    Display *xdpy = gdk_x11_display_get_xdisplay(display);
    XMoveWindow(xdpy, gdk_x11_surface_get_xid(surface), x, y);
    XFlush(xdpy);
}

#pragma GCC diagnostic pop

static gboolean center_cb(gpointer data)
{
    (void)data;
    center_now();
    return G_SOURCE_REMOVE;
}

void gui_window_state_center_after_present(void)
{
    /* 覆盖窗口管理器延迟放置。仅 X11 生效，Wayland 由合成器决定位置。 */
    g_timeout_add(100, center_cb, NULL);
    g_timeout_add(400, center_cb, NULL);
    g_timeout_add(800, center_cb, NULL);
}
