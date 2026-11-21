/*
 * storage_columns.c - 存储列与单元格工厂
 */

#include "storage_internal.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "gui_util.h"

G_DEFINE_TYPE(MountItem, mount_item, G_TYPE_OBJECT)

static void mount_item_finalize(GObject *obj)
{
    MountItem *self = (MountItem *)obj;

    g_free(self->mountpoint);
    g_free(self->device);
    g_free(self->fstype);

    G_OBJECT_CLASS(mount_item_parent_class)->finalize(obj);
}

static void mount_item_class_init(MountItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mount_item_finalize;
}

static void mount_item_init(MountItem *self)
{
    (void)self;
}

/* --------------------------------------------------------------------------
 * 单元格工厂
 * ------------------------------------------------------------------------- */
static void setup_text_cell(GtkSignalListItemFactory *factory,
                            GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(item, label);
}

static void set_level_css(GtkWidget *w, double v, double warn, double bad)
{
    gtk_widget_remove_css_class(w, "health-ok");
    gtk_widget_remove_css_class(w, "health-warn");
    gtk_widget_remove_css_class(w, "health-bad");

    if (v >= bad)
        gtk_widget_add_css_class(w, "health-bad");
    else if (v >= warn)
        gtk_widget_add_css_class(w, "health-warn");
    else
        gtk_widget_add_css_class(w, "health-ok");
}

static void bind_mountpoint(GtkSignalListItemFactory *f, GtkListItem *item,
                            gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           m->mountpoint);
}

static void bind_device(GtkSignalListItemFactory *f, GtkListItem *item,
                        gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           (m->device != NULL) ? m->device : "");
}

static void bind_fstype(GtkSignalListItemFactory *f, GtkListItem *item,
                        gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           (m->fstype != NULL) ? m->fstype : "");
}

static void bind_total(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m == NULL)
        return;

    gchar *txt = fmon_gui_format_bytes(m->total);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), txt);
    g_free(txt);
}

static void bind_used(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m == NULL)
        return;

    guint64 used = (m->total > m->freeb) ? m->total - m->freeb : 0;
    gchar  *txt  = fmon_gui_format_bytes(used);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), txt);
    g_free(txt);
}

static void bind_free(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m == NULL)
        return;

    gchar *txt = fmon_gui_format_bytes(m->freeb);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), txt);
    g_free(txt);
}

static void bind_inode(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);

    if (m == NULL)
        return;

    gchar buf[32];

    if (m->inode_pct >= 0.0)
        g_snprintf(buf, sizeof(buf), "%.0f%%", m->inode_pct);
    else
        g_strlcpy(buf, "—", sizeof(buf));

    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_ro(GtkSignalListItemFactory *f, GtkListItem *item,
                    gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);

    if (m != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           m->ro ? _("只读") : _("读写"));
}

static void setup_bar_cell(GtkSignalListItemFactory *factory,
                           GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *bar = gtk_level_bar_new_for_interval(0.0, 100.0);
    gtk_widget_set_size_request(bar, 120, -1);
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_box_append(GTK_BOX(box), bar);

    GtkWidget *pct = gtk_label_new(NULL);
    gtk_label_set_width_chars(GTK_LABEL(pct), 7);
    gtk_label_set_xalign(GTK_LABEL(pct), 1.0f);
    gtk_widget_set_tooltip_text(
        box, _("容量使用率：≥85% 黄色，≥95% 红色；ZFS ≥80% 即黄色"));
    gtk_box_append(GTK_BOX(box), pct);

    gtk_list_item_set_child(item, box);
}

static void bind_bar(GtkSignalListItemFactory *f, GtkListItem *item,
                     gpointer d)
{
    (void)f;
    (void)d;

    MountItem *m = gtk_list_item_get_item(item);
    if (m == NULL)
        return;

    GtkWidget *box = gtk_list_item_get_child(item);
    GtkWidget *bar = gtk_widget_get_first_child(box);
    GtkWidget *pct = gtk_widget_get_last_child(box);

    gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), m->pct);

    gchar buf[32];
    g_snprintf(buf, sizeof(buf), "%.1f%%", m->pct);
    gtk_label_set_text(GTK_LABEL(pct), buf);

    /* 容量警示：普通 FS 85/95；ZFS 80 起就提示写放大风险 */
    double warn = (strcmp(m->fstype, "zfs") == 0) ? 80.0 : 85.0;

    set_level_css(pct, m->pct, warn, 95.0);

    guint64 used = (m->total > m->freeb) ? m->total - m->freeb : 0;
    char *ut = fmon_gui_format_bytes(used);
    char *ft = fmon_gui_format_bytes(m->freeb);
    char *tt = fmon_gui_format_bytes(m->total);
    gchar *tip = g_strdup_printf(
        _("总容量 %s\n已用 %s\n可用 %s\n使用率 %.1f%%"), tt, ut, ft,
        m->pct);

    gtk_widget_set_tooltip_text(box, tip);
    g_free(tip);
    g_free(ut);
    g_free(ft);
    g_free(tt);
}

/* --------------------------------------------------------------------------
 * 刷新
 * ------------------------------------------------------------------------- */


void storage_columns_attach(GtkColumnView *view)
{
    const char *titles[] = { _("挂载点"), _("设备"), _("类型"), _("总容量"), _("已用"), _("可用"), _("使用率"), _("Inode"), _("状态") };
    GCallback binders[] = { (GCallback)bind_mountpoint, (GCallback)bind_device,
                            (GCallback)bind_fstype, (GCallback)bind_total,
                            (GCallback)bind_used, (GCallback)bind_free,
                            (GCallback)bind_bar, (GCallback)bind_inode,
                            (GCallback)bind_ro };

    for (int i = 0; i < 9; i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

        g_signal_connect(factory, "setup",
                         G_CALLBACK(i == 6 ? setup_bar_cell : setup_text_cell),
                         NULL);
        g_signal_connect(factory, "bind", binders[i], NULL);

        GtkColumnViewColumn *col = gtk_column_view_column_new(titles[i], factory);

        gtk_column_view_column_set_expand(col, (i == 0 || i == 1));
        gtk_column_view_append_column(view, col);
    }
}
