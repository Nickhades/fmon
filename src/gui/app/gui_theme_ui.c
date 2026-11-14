/*
 * gui_theme_ui.c - 主题分段开关（圆角滑动指示块）
 */

#include "gui_theme_ui.h"

#include "gui_refresh.h"
#include "theme.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <math.h>

extern GtkWindow *g_main_window;

static GtkWidget *s_btn_light;
static GtkWidget *s_btn_dark;
static GtkWidget *s_thumb_area;
static gboolean   s_syncing;

static double s_pos;        /* 指示块位置：0=浅色，1=深色 */
static double s_target;
static guint  s_anim_id;

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
                         double r)
{
    if (r > h / 2.0)
        r = h / 2.0;
    if (r > w / 2.0)
        r = w / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_thumb(GtkDrawingArea *area, cairo_t *cr, int width,
                       int height, gpointer data)
{
    (void)area;
    (void)data;

    const double pad = 3.0;                 /* 滑块与轨道之间留空隙 */
    double track_r = height / 2.0;
    double th      = height - 2.0 * pad;
    double hw      = (width - 2.0 * pad) / 2.0;
    double span    = width - 2.0 * pad - hw;
    double x       = pad + s_pos * span;
    gboolean dark  = fmon_theme_is_dark();

    /* 底槽：圆角，浅色/深色用不同的半透明前景 */
    if (dark)
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
    else
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.08);

    rounded_rect(cr, 0, 0, width, height, track_r);
    cairo_fill(cr);

    /* 滑块：背景跟随主题（浅色=白，深色=深灰），内缩留空隙 */
    if (dark)
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);   /* 深色主题：黑色滑块 */
    else
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);   /* 浅色主题：白色滑块 */

    rounded_rect(cr, x, pad, hw, th, th / 2.0);
    cairo_fill(cr);

    /* 图标：直接画在轨道上，颜色由主题决定，避免符号图标染色不生效 */
    double cy  = height / 2.0;
    double cx1 = pad + hw / 2.0;
    double cx2 = width - pad - hw / 2.0;
    double ir  = MIN(th, hw) * 0.16;

    for (int i = 0; i < 2; i++) {
        gboolean active = (i == 0) ? (s_pos < 0.5) : (s_pos >= 0.5);
        double   cx     = (i == 0) ? cx1 : cx2;

        if (active) {
            if (dark)
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
            else
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.90);
        } else {
            if (dark)
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
            else
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
        }

        if (i == 0) {
            /* 太阳 */
            cairo_arc(cr, cx, cy, ir, 0, 2 * G_PI);
            cairo_fill(cr);

            for (int k = 0; k < 8; k++) {
                double a = k * G_PI / 4.0;
                cairo_move_to(cr, cx + cos(a) * ir * 1.55,
                              cy + sin(a) * ir * 1.55);
                cairo_line_to(cr, cx + cos(a) * ir * 2.20,
                              cy + sin(a) * ir * 2.20);
            }
            cairo_set_line_width(cr, 1.4);
            cairo_stroke(cr);
        } else {
            /* 月亮（月牙） */
            cairo_arc(cr, cx, cy, ir * 1.35, G_PI * 0.45, G_PI * 1.55);
            cairo_arc_negative(cr, cx + ir * 0.75, cy, ir * 1.15,
                               G_PI * 1.55, G_PI * 0.45);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }

    /* 描边：浅色白滑块用淡黑边，深色黑滑块用淡白边 */
    cairo_set_line_width(cr, 1.0);

    if (dark)
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
    else
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.10);

    rounded_rect(cr, x + 0.5, pad + 0.5, hw - 1.0, th - 1.0,
                 (th - 1.0) / 2.0);
    cairo_stroke(cr);
}

static gboolean anim_step(gpointer data)
{
    (void)data;

    double diff = s_target - s_pos;

    if (fabs(diff) < 0.02) {
        s_pos = s_target;
        s_anim_id = 0;
        gtk_widget_queue_draw(s_thumb_area);
        return G_SOURCE_REMOVE;
    }

    s_pos += diff * 0.30;
    gtk_widget_queue_draw(s_thumb_area);
    return G_SOURCE_CONTINUE;
}

static void animate_to(double target, gboolean animate)
{
    s_target = target;

    if (!animate || s_thumb_area == NULL) {
        s_pos = target;
        gtk_widget_queue_draw(s_thumb_area);
        return;
    }

    if (s_anim_id == 0)
        s_anim_id = g_timeout_add(16, anim_step, NULL);
}

static void sync_buttons(gboolean animate)
{
    if (s_btn_light == NULL || s_btn_dark == NULL)
        return;

    gboolean dark = (fmon_theme_mode() == FMON_THEME_DARK);

    s_syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_btn_light), !dark);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_btn_dark), dark);
    s_syncing = FALSE;

    animate_to(dark ? 1.0 : 0.0, animate);
}

static void on_theme_changed(void)
{
    sync_buttons(TRUE);
    gui_pages_redraw_all();

    if (g_main_window != NULL)
        gtk_widget_queue_draw(GTK_WIDGET(g_main_window));
}

static void on_mode_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;

    if (s_syncing || !gtk_toggle_button_get_active(btn))
        return;

    if (btn == GTK_TOGGLE_BUTTON(s_btn_light))
        fmon_theme_set(FMON_THEME_LIGHT);
    else
        fmon_theme_set(FMON_THEME_DARK);
}

void gui_theme_ui_init(const char *start_theme)
{
    static gboolean ready = FALSE;

    if (ready)
        return;

    fmon_theme_init();
    fmon_theme_set_observer(on_theme_changed);

    if (start_theme != NULL) {
        if (g_strcmp0(start_theme, "light") == 0)
            fmon_theme_set(FMON_THEME_LIGHT);
        else if (g_strcmp0(start_theme, "dark") == 0)
            fmon_theme_set(FMON_THEME_DARK);
    }

    /* 只有浅色 / 深色两种：auto 统一落到浅色 */
    if (fmon_theme_mode() != FMON_THEME_LIGHT &&
        fmon_theme_mode() != FMON_THEME_DARK)
        fmon_theme_set(FMON_THEME_LIGHT);

    ready = TRUE;
}

GtkWidget *gui_theme_ui_switch(void)
{
    GtkWidget *overlay = gtk_overlay_new();

    gtk_widget_add_css_class(overlay, "theme-switch");
    /* 不用 overflow:hidden：GtkOverlay 会按矩形裁剪，导致轨道右端圆角被切平 */
    gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(overlay, 84, 34);
    gtk_widget_set_hexpand(overlay, FALSE);
    gtk_widget_set_vexpand(overlay, FALSE);

    /* 背景层：底槽 + 滑动指示块（画在按钮下面） */
    s_thumb_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_thumb_area, 84, 34);
    gtk_widget_set_hexpand(s_thumb_area, FALSE);
    gtk_widget_set_vexpand(s_thumb_area, FALSE);
    gtk_widget_set_halign(s_thumb_area, GTK_ALIGN_FILL);
    gtk_widget_set_valign(s_thumb_area, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(s_thumb_area, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_thumb_area), draw_thumb,
                                   NULL, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), s_thumb_area);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(box, GTK_ALIGN_FILL);


    s_btn_light = gtk_toggle_button_new();
    gtk_widget_add_css_class(s_btn_light, "theme-seg");
    gtk_widget_set_tooltip_text(s_btn_light, _("浅色"));
    gtk_widget_set_size_request(s_btn_light, 42, 32);
    g_signal_connect(s_btn_light, "toggled", G_CALLBACK(on_mode_toggled), NULL);

    s_btn_dark = gtk_toggle_button_new();
    gtk_widget_add_css_class(s_btn_dark, "theme-seg");
    gtk_widget_set_tooltip_text(s_btn_dark, _("深色"));
    gtk_widget_set_size_request(s_btn_dark, 42, 32);
    g_signal_connect(s_btn_dark, "toggled", G_CALLBACK(on_mode_toggled), NULL);

    /* 互斥：点已选中的一侧不会把两个都取消 */
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(s_btn_dark),
                                GTK_TOGGLE_BUTTON(s_btn_light));

    gtk_box_append(GTK_BOX(box), s_btn_light);
    gtk_box_append(GTK_BOX(box), s_btn_dark);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box);

    sync_buttons(FALSE);
    return overlay;
}













