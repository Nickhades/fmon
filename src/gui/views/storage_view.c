/*
 * storage_view.c - 存储空间（挂载点 / 文件系统）页面
 *
 * 这是本次新增的功能：资源管理器里应该能一眼看到“哪个分区快满了”。
 * 数据来自守护进程的 getmntinfo()/statfs(2) 采样，界面只读快照，
 * 不执行任何 df/mount 子进程。
 */

#include <glib/gi18n.h>

#include <sys/mount.h>

#include "disk_history.h"
#include "disk_perf.h"
#include "storage_view.h"

#include "gui_state.h"
#include "gui_util.h"

typedef struct {
    GObject parent_instance;

    gchar   *mountpoint;
    gchar   *device;
    gchar   *fstype;
    guint64  total;
    guint64  freeb;
    gdouble  pct;
    gdouble  inode_pct;   /* <0 表示文件系统不提供 inode 统计 */
    gboolean ro;
} MountItem;

typedef struct {
    GObjectClass parent_class;
} MountItemClass;

GType mount_item_get_type(void);
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
static void rebuild_store(GListStore *store)
{
    const FmonShmData *s = gui_state_snapshot();

    g_list_store_remove_all(store);

    uint32_t n = s->mount_count;
    if (n > FMON_MAX_MOUNTS)
        n = FMON_MAX_MOUNTS;

    for (uint32_t i = 0; i < n; i++) {
        const FmonMountInfo *mi = &s->mounts[i];

        MountItem *item = g_object_new(mount_item_get_type(), NULL);
        item->mountpoint = g_strdup(mi->mountpoint);
        item->device     = g_strdup(mi->device);
        item->fstype     = g_strdup(mi->fstype);
        item->total      = mi->total_bytes;
        item->freeb      = mi->free_bytes;
        item->pct        = mi->used_pct;
        item->inode_pct  = -1.0;
        item->ro         = FALSE;

        /* inode 与只读标志由 statfs(2) 直接读取，不经过 daemon */
        struct statfs sfs;

        if (statfs(mi->mountpoint, &sfs) == 0) {
            if (sfs.f_files > 0)
                item->inode_pct =
                    100.0 * (double)(sfs.f_files - sfs.f_ffree) /
                    (double)sfs.f_files;

            item->ro = ((sfs.f_flags & MNT_RDONLY) != 0);
        }

        g_list_store_append(store, item);
        g_object_unref(item);
    }
}

static void remove_source(gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);
    if (id != 0)
        g_source_remove(id);
}

static GtkWidget *g_storage_box = NULL;
static GtkWidget *g_disk_cells[FMON_MAX_DISKS][12];
static GtkWidget *g_disk_rows[FMON_MAX_DISKS];

static void on_device_clicked(GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer data)
{
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;

    int r = GPOINTER_TO_INT(data);

    if (r < 0 || r >= FMON_MAX_DISKS)
        return;

    const char *dev = gtk_label_get_text(GTK_LABEL(g_disk_cells[r][0]));

    if (dev[0] != '\0')
        disk_history_open_dialog(dev, GTK_WINDOW(fmon_gui_main_window()));
}

static void on_history_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;

    int r = GPOINTER_TO_INT(data);

    if (r < 0 || r >= FMON_MAX_DISKS)
        return;

    const char *dev = gtk_label_get_text(GTK_LABEL(g_disk_cells[r][0]));

    if (dev[0] != '\0')
        disk_history_open_dialog(dev, GTK_WINDOW(fmon_gui_main_window()));
}

/* 每块磁盘的独立速率表（数据来自守护进程的 disks[]） */
static void update_disk_table(void)
{
    const FmonShmData *s = gui_state_snapshot();
    uint32_t           n  = s->disk_count;
    char               tmp[64];

    if (n > FMON_MAX_DISKS)
        n = FMON_MAX_DISKS;

    for (uint32_t r = 0; r < n; r++) {
        const FmonDiskInfo *d = &s->disks[r];

        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][0]), d->name);

        char *rb = fmon_gui_format_rate(d->read_kbps);
        char *wb = fmon_gui_format_rate(d->write_kbps);
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][1]), rb);
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][2]), wb);
        g_free(rb);
        g_free(wb);

        gchar *ops = g_strdup_printf("%llu", (unsigned long long)d->read_ops);
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][3]), ops);
        g_free(ops);

        ops = g_strdup_printf("%llu", (unsigned long long)d->write_ops);
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][4]), ops);
        g_free(ops);

        gchar *busy = g_strdup_printf("%.0f%%", d->busy_pct);
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][5]), busy);
        g_free(busy);

        /* 延迟/队列/SMART 来自 disk_perf（当前主设备），只匹配同名盘 */
        const DiskPerf *p  = disk_perf_get();
        gboolean        ok = (p != NULL && p->valid &&
                              strcmp(p->dev, d->name) == 0);

        if (ok)
            g_snprintf(tmp, sizeof(tmp), "%.2f ms", p->latency_ms);
        else
            g_strlcpy(tmp, "—", sizeof(tmp));
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][6]), tmp);

        if (ok)
            g_snprintf(tmp, sizeof(tmp), "%.1f", p->queue_len);
        else
            g_strlcpy(tmp, "—", sizeof(tmp));
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][7]), tmp);

        uint64_t nops = d->read_ops + d->write_ops;
        uint64_t nbyt = d->read_bytes + d->write_bytes;

        if (nops > 0)
            g_snprintf(tmp, sizeof(tmp), "%.0f KB",
                       (double)nbyt / (double)nops / 1024.0);
        else
            g_strlcpy(tmp, "—", sizeof(tmp));
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][8]), tmp);

        if (d->smart_valid != 0 && d->temp_c > 0.0) {
            g_snprintf(tmp, sizeof(tmp), "%.1f°C", d->temp_c);
            set_level_css(g_disk_cells[r][9], d->temp_c, 60.0, 70.0);
        } else {
            g_strlcpy(tmp, "—", sizeof(tmp));
            gtk_widget_remove_css_class(g_disk_cells[r][9], "health-ok");
            gtk_widget_remove_css_class(g_disk_cells[r][9], "health-warn");
            gtk_widget_remove_css_class(g_disk_cells[r][9], "health-bad");
        }
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][9]), tmp);

        if (d->smart_valid != 0 && d->wear_pct >= 0)
            g_snprintf(tmp, sizeof(tmp), "%d%%", d->wear_pct);
        else
            g_strlcpy(tmp, "—", sizeof(tmp));
        gtk_label_set_text(GTK_LABEL(g_disk_cells[r][10]), tmp);
    }

    for (uint32_t r = 0; r < FMON_MAX_DISKS; r++)
        gtk_widget_set_visible(g_disk_rows[r], r < n);
}

static GtkWidget *build_disk_tab(void)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);

    const char *headers[] = { "设备", "读", "写", "读次", "写次", "繁忙",
                              "延迟", "队列", "块大小", "温度", "寿命",
                              "历史" };
    /* 固定列宽：刷新时数字长短变化不会把表头挤来挤去 */
    static const int widths[] = { 60, 78, 78, 86, 86, 52,
                                  72, 52, 72, 62, 48, 52 };

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

    gtk_widget_add_css_class(header, "disk-header");

    for (int c = 0; c < 12; c++) {
        GtkWidget *h = gtk_label_new(headers[c]);

        gtk_widget_add_css_class(h, "small-text");
        gtk_label_set_xalign(GTK_LABEL(h), 0.0f);
        gtk_widget_set_halign(h, GTK_ALIGN_START);
        gtk_widget_set_size_request(h, widths[c], -1);
        gtk_box_append(GTK_BOX(header), h);
    }

    gtk_box_append(GTK_BOX(vbox), header);

    static const char *const tips[] = {
        "设备名（点击查看 24 小时历史）", "当前读取速率", "当前写入速率",
        "累计读次数",
        "累计写次数", "设备繁忙百分比",
        "平均 I/O 延迟（devstat，毫秒）",
        "当前 I/O 队列长度（排队请求数）",
        "累计字节 ÷ 累计操作数，反映平均 I/O 尺寸",
        "NVMe 温度（SMART）；≥60°C 黄、≥70°C 红",
        "NVMe 已用寿命百分比（SMART）",
    };

    for (int r = 0; r < FMON_MAX_DISKS; r++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

        gtk_widget_add_css_class(row, "disk-row");
        g_disk_rows[r] = row;

        for (int c = 0; c < 12; c++) {
            if (c == 11) {
                GtkWidget *btn = gtk_button_new_with_label(_("历史"));

                gtk_widget_add_css_class(btn, "flat");
                gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
                gtk_widget_set_tooltip_text(
                    btn, _("查看该设备 24 小时延迟 / 吞吐 / 温度"));
                g_signal_connect(btn, "clicked",
                                 G_CALLBACK(on_history_clicked),
                                 GINT_TO_POINTER(r));
                gtk_widget_set_size_request(btn, widths[c], -1);
                gtk_box_append(GTK_BOX(row), btn);
                g_disk_cells[r][c] = btn;
                continue;
            }

            GtkWidget *cell = gtk_label_new("");

            gtk_label_set_xalign(GTK_LABEL(cell), 0.0f);
            gtk_widget_set_halign(cell, GTK_ALIGN_START);
            gtk_widget_set_valign(cell, GTK_ALIGN_CENTER);
            gtk_widget_set_size_request(cell, widths[c], -1);
            gtk_widget_set_tooltip_text(cell, tips[c]);
            if (c == 0) {
                GtkGesture *gesture = gtk_gesture_click_new();

                gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
                g_signal_connect(gesture, "pressed",
                                 G_CALLBACK(on_device_clicked),
                                 GINT_TO_POINTER(r));
                gtk_widget_add_controller(cell,
                                          GTK_EVENT_CONTROLLER(gesture));
                gtk_widget_set_cursor_from_name(cell, "pointer");
            }

            gtk_box_append(GTK_BOX(row), cell);
            g_disk_cells[r][c] = cell;
        }

        gtk_box_append(GTK_BOX(vbox), row);
    }

    GtkWidget *hint = gtk_label_new(
        "速率/延迟/队列来自 devstat(3)；块大小为累计字节/次数；"
        "温度与寿命来自内核 SMART（NVMe ioctl / ATA / SCSI CAM，无传感器时显示 —）。");
    gtk_widget_add_css_class(hint, "small-text");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hint, 8);
    gtk_box_append(GTK_BOX(vbox), hint);

    update_disk_table();
    return vbox;
}

static gboolean on_refresh_tick(gpointer data)
{
    GListStore *store = G_LIST_STORE(data);

    if (fmon_gui_main_window() == NULL)
        return G_SOURCE_REMOVE;

    /* 页面不可见时跳过（GtkStack 只 map 当前页） */
    if (g_storage_box == NULL || !gtk_widget_get_mapped(g_storage_box))
        return G_SOURCE_CONTINUE;

    if (!gui_state_online())
        return G_SOURCE_CONTINUE;

    rebuild_store(store);
    update_disk_table();
    return G_SOURCE_CONTINUE;
}

static void on_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    rebuild_store(G_LIST_STORE(data));
}

GtkWidget *gui_storage_view_new(void)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(vbox, "view-card");
    gtk_widget_set_vexpand(vbox, TRUE);
    g_storage_box = vbox;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *btn = gtk_button_new_with_label(_("刷新"));
    gtk_widget_add_css_class(btn, "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), btn);

    GtkWidget *hint = gtk_label_new(_("容量统计来自 statfs(2)，与 df 的 Use% 口径一致"));
    gtk_widget_add_css_class(hint, "small-text");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(toolbar), hint);

    gtk_box_append(GTK_BOX(vbox), toolbar);

    disk_history_start();

    GListStore *store = g_list_store_new(mount_item_get_type());
    GtkSingleSelection *selection =
        gtk_single_selection_new(G_LIST_MODEL(store));
    gtk_single_selection_set_autoselect(selection, FALSE);

    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);

    const char *titles[] = {_("挂载点"), _("设备"), _("类型"), _("总容量"), _("已用"), _("可用"), _("使用率"), _("Inode"), _("状态")};
    GCallback binders[]  = { (GCallback)bind_mountpoint,
                             (GCallback)bind_device,
                             (GCallback)bind_fstype,
                             (GCallback)bind_total,
                             (GCallback)bind_used,
                             (GCallback)bind_free,
                             (GCallback)bind_bar,
                             (GCallback)bind_inode,
                             (GCallback)bind_ro };

    for (int i = 0; i < 9; i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_signal_connect(factory, "setup",
                         G_CALLBACK(i == 6 ? setup_bar_cell : setup_text_cell),
                         NULL);
        g_signal_connect(factory, "bind", binders[i], NULL);

        GtkColumnViewColumn *col = gtk_column_view_column_new(titles[i], factory);
        gtk_column_view_column_set_expand(col, (i == 0 || i == 1));
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);

    /* 两个标签页：挂载点 + 每块磁盘的独立速率 */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll,
                             gtk_label_new("挂载点"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_disk_tab(),
                             gtk_label_new("磁盘设备"));

    gtk_box_append(GTK_BOX(vbox), notebook);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_refresh_clicked), store);

    /* 定时器使用 store，因此由 vbox 持有引用直到控件销毁 */
    /* 必须自己持有一个引用：store 可能已经被 selection 模型接管，
     * 直接把手上的指针交给销毁函数会导致多一次 unref（退出时崩溃）。 */
    g_object_set_data_full(G_OBJECT(vbox), "mount-store",
                           g_object_ref(store), g_object_unref);

    guint timer = g_timeout_add(2000, on_refresh_tick, store);
    g_object_set_data_full(G_OBJECT(vbox), "mount-timer",
                           GUINT_TO_POINTER(timer), remove_source);

    return vbox;
}



