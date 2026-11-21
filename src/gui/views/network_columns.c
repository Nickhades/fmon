/*
 * network_columns.c - 网络连接/接口列与单元格工厂
 */

#include "network_internal.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "gui_util.h"



typedef struct {
    GObjectClass parent_class;
} NetItemClass;

GType net_item_get_type(void);
G_DEFINE_TYPE(NetItem, net_item, G_TYPE_OBJECT)

static void net_item_finalize(GObject *obj)
{
    NetItem *self = (NetItem *)obj;

    g_free(self->user);
    g_free(self->command);
    g_free(self->proto);
    g_free(self->local);
    g_free(self->foreign);
    g_free(self->state);
    g_free(self->service);
    g_free(self->iface);

    G_OBJECT_CLASS(net_item_parent_class)->finalize(obj);
}

static void net_item_class_init(NetItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = net_item_finalize;
}

static void net_item_init(NetItem *self)
{
    (void)self;
}

/* ===========================================================================
 * 接口条目
 * ========================================================================= */


typedef struct {
    GObjectClass parent_class;
} IfaceItemClass;

GType iface_item_get_type(void);
G_DEFINE_TYPE(IfaceItem, iface_item, G_TYPE_OBJECT)

static void iface_item_finalize(GObject *obj)
{
    IfaceItem *self = (IfaceItem *)obj;

    g_free(self->name);
    g_free(self->addr);
    g_free(self->state);

    G_OBJECT_CLASS(iface_item_parent_class)->finalize(obj);
}

static void iface_item_class_init(IfaceItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = iface_item_finalize;
}

static void iface_item_init(IfaceItem *self)
{
    (void)self;
}

/* ===========================================================================
 * 单元格工厂
 * ========================================================================= */
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

static void bind_user(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);
    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), n->user);
}

static void bind_command(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);
    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           n->command);
}

static void bind_proto(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);
    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), n->proto);
}

static void bind_local(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);
    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), n->local);
}

static void bind_foreign(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);
    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           n->foreign);
}

static void bind_uid(GtkSignalListItemFactory *f, GtkListItem *item,
                      gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);

    if (n == NULL)
        return;

    gchar buf[16];

    g_snprintf(buf, sizeof(buf), "%u", n->uid);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_state(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);

    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           (n->state[0] != '\0') ? n->state : "—");
}

static void bind_service(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);

    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                           n->service);
}

static void bind_iface(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);

    if (n != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), n->iface);
}

static void bind_fd(GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);

    if (n == NULL)
        return;

    gchar buf[16];

    g_snprintf(buf, sizeof(buf), "%u", n->fd);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_duration(GtkSignalListItemFactory *f, GtkListItem *item,
                          gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);

    if (n == NULL)
        return;

    gint64 secs = (g_get_monotonic_time() - n->since_us) / G_TIME_SPAN_SECOND;

    if (secs < 0)
        secs = 0;

    gchar buf[32];

    if (secs < 60)
        g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT "s", secs);
    else if (secs < 3600)
        g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT "m%02" G_GINT64_FORMAT
                                        "s", secs / 60, secs % 60);
    else
        g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT "h%02" G_GINT64_FORMAT
                                        "m", secs / 3600, (secs % 3600) / 60);

    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_pid(GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    (void)f;
    (void)d;

    NetItem *n = gtk_list_item_get_item(item);
    if (n == NULL)
        return;

    gchar buf[16];
    g_snprintf(buf, sizeof(buf), "%u", n->pid);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_if_name(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), i->name);
}

static void bind_if_addr(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), i->addr);
}

static void bind_if_state(GtkSignalListItemFactory *f, GtkListItem *item,
                          gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i != NULL)
        gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), i->state);
}


static void bind_if_mtu(GtkSignalListItemFactory *f, GtkListItem *item,
                        gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i == NULL)
        return;

    gchar buf[16];
    g_snprintf(buf, sizeof(buf), "%u", i->mtu);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

static void bind_if_rx(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i == NULL)
        return;

    gchar *txt = fmon_gui_format_rate(i->rx);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), txt);
    g_free(txt);
}

static void bind_if_tx(GtkSignalListItemFactory *f, GtkListItem *item,
                       gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i == NULL)
        return;

    gchar *txt = fmon_gui_format_rate(i->tx);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), txt);
    g_free(txt);
}

static void bind_if_errors(GtkSignalListItemFactory *f, GtkListItem *item,
                           gpointer d)
{
    (void)f;
    (void)d;

    IfaceItem *i = gtk_list_item_get_item(item);
    if (i == NULL)
        return;

    gchar buf[32];
    g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT, i->errors);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), buf);
}

/* ===========================================================================
 * 解析 sockstat + netstat 输出
 * ========================================================================= */
static int conn_compare(gconstpointer a, gconstpointer b, gpointer data)
{
    const NetItem *x = a;
    const NetItem *y = b;
    int col = GPOINTER_TO_INT(data);

    switch (col) {
    case 1:
        return (x->uid > y->uid) - (x->uid < y->uid);
    case 3:
        return (x->pid > y->pid) - (x->pid < y->pid);
    case 5:
        return g_strcmp0(x->state, y->state);
    case 6:
        return g_strcmp0(x->service, y->service);
    case 9:
        return g_strcmp0(x->iface, y->iface);
    case 10:
        return (x->fd > y->fd) - (x->fd < y->fd);
    case 11:
        return (x->since_us > y->since_us) - (x->since_us < y->since_us);
    case 0:
        return g_strcmp0(x->user, y->user);
    case 2:
        return g_strcmp0(x->command, y->command);
    case 4:
        return g_strcmp0(x->proto, y->proto);
    case 7:
        return g_strcmp0(x->local, y->local);
    default:
        return g_strcmp0(x->foreign, y->foreign);
    }
}


void network_conn_columns_attach(GtkColumnView *view)
{
    const char *titles[] = { _("用户"), _("UID"), _("命令"), "PID",
                             _("协议"), _("状态"), _("服务"), _("本地地址"),
                             _("远端地址"), _("接口"), "FD", _("持续") };
    GCallback binders[] = { (GCallback)bind_user, (GCallback)bind_uid,
                            (GCallback)bind_command, (GCallback)bind_pid,
                            (GCallback)bind_proto, (GCallback)bind_state,
                            (GCallback)bind_service, (GCallback)bind_local,
                            (GCallback)bind_foreign, (GCallback)bind_iface,
                            (GCallback)bind_fd, (GCallback)bind_duration };

    for (int i = 0; i < 12; i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

        g_signal_connect(factory, "setup", G_CALLBACK(setup_text_cell), NULL);
        g_signal_connect(factory, "bind", binders[i], NULL);

        GtkColumnViewColumn *col = gtk_column_view_column_new(titles[i], factory);

        gtk_column_view_column_set_expand(col, (i == 7 || i == 8));
        gtk_column_view_column_set_sorter(
            col, GTK_SORTER(gtk_custom_sorter_new(conn_compare,
                                                  GINT_TO_POINTER(i), NULL)));
        gtk_column_view_append_column(view, col);
    }
}

void network_iface_columns_attach(GtkColumnView *view)
{
    const char *titles[] = { _("接口"), _("地址"), _("状态"), "MTU",
                             _("接收"), _("发送"), _("错误") };
    GCallback binders[] = { (GCallback)bind_if_name, (GCallback)bind_if_addr,
                            (GCallback)bind_if_state, (GCallback)bind_if_mtu,
                            (GCallback)bind_if_rx, (GCallback)bind_if_tx,
                            (GCallback)bind_if_errors };

    for (int i = 0; i < 7; i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

        g_signal_connect(factory, "setup", G_CALLBACK(setup_text_cell), NULL);
        g_signal_connect(factory, "bind", binders[i], NULL);

        GtkColumnViewColumn *col = gtk_column_view_column_new(titles[i], factory);

        gtk_column_view_column_set_expand(col, (i == 1));
        gtk_column_view_append_column(view, col);
    }
}
