/*
 * about.c - 「关于 / 许可证」对话框实现
 *
 * 两个页签：
 *   1. 本软件：BSD 2-Clause 全文；
 *   2. 第三方组件：程序实际链接/调用的库及其许可证摘要。
 *
 * 许可证文本内嵌在程序里，保证即使 LICENSE 文件没有被安装到
 * <前缀>/share/licenses/fmon，界面里也能完整查看协议。
 */

#include "about.h"

#include "fmon_shm.h"   /* FMON_LOCALBASE */

#include <glib/gi18n.h>

#include "fmon_shm.h"

/* 与项目根目录的 LICENSE 文件保持一致的 BSD 2-Clause 文本 */
static const char *const license_text =
    "BSD 2-Clause License\n"
    "\n"
    "Copyright (c) 2026, Zhang Xin Yu\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are met:\n"
    "\n"
    "1. Redistributions of source code must retain the above copyright notice, this\n"
    "   list of conditions and the following disclaimer.\n"
    "\n"
    "2. Redistributions in binary form must reproduce the above copyright notice,\n"
    "   this list of conditions and the following disclaimer in the documentation\n"
    "   and/or other materials provided with the distribution.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
    "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
    "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\n"
    "DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"
    "FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"
    "DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"
    "SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"
    "CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,\n"
    "OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n";

/* 第三方组件摘要（版本号在运行时填充，保证与实际链接的库一致） */
static gchar *build_third_party_text(void)
{
    return g_strdup_printf(
        "本程序使用了以下第三方组件，其版权归各自作者所有。\n"
        "本程序只动态链接它们，不重新分发这些库的源码。\n"
        "\n"
        "── 界面 ─────────────────────────────────────────────\n"
        "GTK %d.%d.%d — LGPL-2.1-or-later\n"
        "  Copyright (C) The GTK Team\n"
        "  https://www.gtk.org\n"
        "\n"
        "GLib %u.%u.%u — LGPL-2.1-or-later\n"
        "  Copyright (C) GNOME Foundation\n"
        "  https://www.gtk.org\n"
        "\n"
        "Pango — LGPL-2.1-or-later（GTK 的文本排版依赖）\n"
        "  Copyright (C) Owen Taylor, Red Hat, Inc.\n"
        "\n"
        "cairo %s — LGPL-2.1 或 MPL-1.1（二选一）\n"
        "  Copyright (C) Carl Worth 及贡献者\n"
        "  https://www.cairographics.org\n"
        "\n"
        "── 采集 ─────────────────────────────────────────────\n"
        "libkvm / libdevstat（FreeBSD 基础系统）— BSD-2-Clause\n"
        "  Copyright (C) The FreeBSD Project\n"
        "\n"
        "libdrm / libdrm_amdgpu（AMD 显卡传感器，可选）— MIT\n"
        "  Copyright (C) the libdrm authors\n"
        "  https://gitlab.freedesktop.org/mesa/drm\n"
        "\n"
        "── 运行时调用的外部命令 ─────────────────────────────\n"
        "ps、sockstat、zpool、sysrc、service 等来自 FreeBSD 基础系统（BSD）；\n"
        "nvidia-smi 来自 NVIDIA 驱动（专有软件）。\n"
        "这些程序由系统提供，本程序只调用、不重新分发。\n"
        "\n"
        "各软件包的完整许可证文本通常位于：\n"
        "  " FMON_LOCALBASE "/share/licenses/<包名>/COPYING\n"
        "FreeBSD 基础系统的许可证位于 /usr/share/ 与源码树中。\n"
        "\n"
        "本程序自身的许可证（BSD 2-Clause）见「本软件」页签，\n"
        "安装后也位于 " FMON_LOCALBASE "/share/licenses/fmon/LICENSE。\n",
        gtk_get_major_version(), gtk_get_minor_version(),
        gtk_get_micro_version(), glib_major_version, glib_minor_version,
        glib_micro_version, cairo_version_string());
}

static GtkWidget *make_text_page(const char *text)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_css_class(scroll, "license-box");

    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(view, "license-text");

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buf, text, -1);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    return scroll;
}

static void on_close_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    gtk_window_destroy(GTK_WINDOW(data));
}

void fmon_about_show(GtkWindow *parent)
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), _("关于 FreeBSD 监控中心"));
    gtk_window_set_default_size(GTK_WINDOW(win), 620, 640);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(vbox, 18);
    gtk_widget_set_margin_bottom(vbox, 18);
    gtk_widget_set_margin_start(vbox, 18);
    gtk_widget_set_margin_end(vbox, 18);

    GtkWidget *title = gtk_label_new("FreeBSD 监控中心 · fmon");
    gtk_widget_add_css_class(title, "header");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), title);

    gchar *ver = g_strdup_printf(_("版本 %s · 共享内存 ABI %u"),
                                 FMON_VERSION_STRING, FMON_ABI_VERSION);
    GtkWidget *ver_label = gtk_label_new(ver);
    gtk_widget_add_css_class(ver_label, "small-text");
    gtk_widget_set_halign(ver_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), ver_label);
    g_free(ver);

    GtkWidget *copyright = gtk_label_new("Copyright (c) 2026, Zhang Xin Yu");
    gtk_widget_add_css_class(copyright, "small-text");
    gtk_widget_set_halign(copyright, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), copyright);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             make_text_page(license_text),
                             gtk_label_new(_("本软件（BSD 2-Clause）")));

    gchar *third = build_third_party_text();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             make_text_page(third),
                             gtk_label_new(_("第三方组件")));
    g_free(third);

    gtk_box_append(GTK_BOX(vbox), notebook);

    GtkWidget *close = gtk_button_new_with_label(_("关闭"));
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_widget_add_css_class(close, "suggested-action");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), win);
    gtk_box_append(GTK_BOX(vbox), close);

    gtk_window_set_child(GTK_WINDOW(win), vbox);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
    gtk_window_present(GTK_WINDOW(win));
}
