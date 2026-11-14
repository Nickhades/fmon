/*
 * collector_io.c - 磁盘吞吐（devstat）
 *
 * 旧版本用
 *     popen("iostat -d -K 1 2 | awk '...'")
 * 每 100ms 执行一次：既要 fork 两个进程 + awk，iostat 自己还要 sleep 1 秒。
 * 结果是采集线程几乎永远在阻塞，CPU 占用高，而且 awk 把所有分区列都加在
 * 一起，数值会被重复计算。
 *
 * 现在改为直接使用 FreeBSD 的 devstat(3) API（链接 -ldevstat）：
 *   1. 不创建任何子进程；
 *   2. 只统计整盘设备（跳过 DEVSTAT_TYPE_PASS 直通设备），不会重复计数；
 *   3. 速率用“本次累计 - 上次累计”除以真实经过的时间，结果准确。
 *
 * GPU 采集已拆分到 collector_gpu.c（支持多显卡自动探测与切换）。
 */

#include "fmon_daemon.h"

#include "fmon_child.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include "fmon_sysctl.h"

#include <devstat.h>
#include <stdlib.h>

/* libdevstat 的内部缓冲区由 devstat_getdevs() 分配，需要调用者释放。
 * 放在文件作用域，线程退出时统一清理。 */
static struct statinfo g_devstat;

static void devstat_release(void)
{
    if (g_devstat.dinfo == NULL)
        return;

    free(g_devstat.dinfo->mem_ptr);
    free(g_devstat.dinfo);
    g_devstat.dinfo = NULL;
}

/* ===========================================================================
 * 磁盘
 * ========================================================================= */

/* devstat 上报的是驱动名（如 nda），/dev 下是带单元号的节点（nda0）。
 * 用 kern.disks 把它们对应起来。 */
static void map_disk_name(const char *devstat_name, char *out, size_t out_len)
{
    size_t len = 0;
    char  *disks = NULL;

    if (sysctlbyname("kern.disks", NULL, &len, NULL, 0) == 0 && len > 0) {
        disks = malloc(len);

        if (disks != NULL &&
            sysctlbyname("kern.disks", disks, &len, NULL, 0) == 0) {
            char  *save = NULL;
            size_t plen = strlen(devstat_name);

            for (char *tok = strtok_r(disks, " ", &save); tok != NULL;
                 tok = strtok_r(NULL, " ", &save)) {
                if (strncmp(tok, devstat_name, plen) == 0 &&
                    isdigit((unsigned char)tok[plen])) {
                    strlcpy(out, tok, out_len);
                    free(disks);
                    return;
                }
            }
        }

        free(disks);
    }

    snprintf(out, out_len, "%s0", devstat_name);
}

/* 磁盘型号/容量/类型：每块盘每 60 秒查一次 diskinfo，结果缓存 */
typedef struct {
    char     name[16];
    char     model[64];
    char     serial[32];
    uint64_t size;
    uint32_t rpm;
    int      rpm_known;   /* diskinfo 的 Rotation rate 是否为确定值 */
    uint32_t kind;
    int64_t  updated_ms;
    int64_t  cam_ms;      /* 上次 camcontrol 富化时间 */
    uint64_t last_size;   /* 用于检测换盘（容量变化则重新探测） */
} DiskMeta;

static DiskMeta g_meta[FMON_MAX_DISKS];

static DiskMeta *meta_slot(const char *name)
{
    for (int i = 0; i < FMON_MAX_DISKS; i++) {
        if (g_meta[i].name[0] != '\0' && strcmp(g_meta[i].name, name) == 0)
            return &g_meta[i];
    }

    for (int i = 0; i < FMON_MAX_DISKS; i++) {
        if (g_meta[i].name[0] == '\0') {
            strlcpy(g_meta[i].name, name, sizeof(g_meta[i].name));
            return &g_meta[i];
        }
    }

    return NULL;
}

/* USB/SATA 桥接盘：diskinfo 常报 Rotation rate=Unknown，
 * 用 root 权限的 camcontrol identify 读取 ATA 的转速字段。
 * 返回 FMON_DISK_*，失败返回 -1。 */
static int cam_rotation_kind(const char *name)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "camcontrol identify %s 2>/dev/null", name);

    FILE *fp = popen(cmd, "r");

    if (fp == NULL)
        return -1;

    char line[256];
    int  kind = -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = strcasestr(line, "rotation rate");

        if (p == NULL)
            continue;

        if (strcasestr(p, "solid") != NULL) {
            kind = FMON_DISK_SSD;
            break;
        }

        char *q = p;

        while (*q != '\0' && !isdigit((unsigned char)*q))
            q++;

        if (isdigit((unsigned char)*q)) {
            unsigned long v = strtoul(q, NULL, 10);

            kind = (v > 0) ? FMON_DISK_HDD : FMON_DISK_SSD;
        }

        break;
    }

    pclose(fp);
    return kind;
}

/* kern.cam.<dev>.<unit>.rotating：1=机械，0=固态，-1=不存在 */
static int cam_rotating(const char *name)
{
    const char *d = name;

    while (*d != '\0' && !isdigit((unsigned char)*d))
        d++;

    if (*d == '\0')
        return -1;

    char oid[64];

    snprintf(oid, sizeof(oid), "kern.cam.%s.rotating", name);

    int    v = 0;
    size_t l = sizeof(v);

    if (sysctlbyname(oid, &v, &l, NULL, 0) != 0)
        return -1;

    return v;
}

/* 最近 10 秒内是否存在 USB 大容量存储设备（root 才能读 usbconfig） */
/* 返回：0=没有 USB 存储，1=USB2，2=USB3。
 * usbconfig 的 spd=SUPER 表示 5Gbps，HIGH/FULL 表示 480/12Mbps。 */
static int usb_storage_speed(int64_t now)
{
    static int64_t last;
    static int     speed;

    if (last != 0 && now - last < 10000)
        return speed;

    last = now;
    speed = 0;

    FILE *fp = popen("usbconfig list 2>/dev/null", "r");

    if (fp != NULL) {
        char line[256];

        while (fgets(line, sizeof(line), fp) != NULL) {
            char *id = strstr(line, "ugen");

            if (id == NULL || strstr(line, "spd=") == NULL)
                continue;

            char dev[32];
            int  n = 0;

            while (id[n] != '\0' && id[n] != ' ' && id[n] != ',' &&
                   n < (int)sizeof(dev) - 1) {
                dev[n] = id[n];
                n++;
            }

            dev[n] = '\0';

            /* 用接口类判断是不是大容量存储：bInterfaceClass = 0x08 */
            char cmd[160];

            snprintf(cmd, sizeof(cmd),
                     "usbconfig -d %s dump_curr_config_desc 2>/dev/null",
                     dev);

            FILE *fp2 = popen(cmd, "r");
            int   storage = 0;

            if (fp2 != NULL) {
                char l2[256];

                while (fgets(l2, sizeof(l2), fp2) != NULL) {
                    if (strcasestr(l2, "bInterfaceClass = 0x08") != NULL ||
                        strcasestr(l2, "bInterfaceClass = 8") != NULL ||
                        strcasestr(l2, "Mass Storage") != NULL) {
                        storage = 1;
                        break;
                    }
                }

                pclose(fp2);
            }

            if (!storage)
                continue;

            if (strcasestr(line, "spd=SUPER") != NULL) {
                speed = 2;
                break;
            }

            if (strcasestr(line, "spd=HIGH") != NULL ||
                strcasestr(line, "spd=FULL") != NULL ||
                strcasestr(line, "spd=LOW") != NULL)
                speed = 1;
        }

        pclose(fp);
    }

    return speed;
}

static void finalize_kind(DiskMeta *m)
{
    int rot = cam_rotating(m->name);

    if (rot >= 0) {
        m->rpm       = (uint32_t)rot;
        m->rpm_known = 1;
    }

    if (m->rpm_known) {
        m->kind = (m->rpm > 0) ? FMON_DISK_HDD : FMON_DISK_SSD;
    } else if (strncmp(m->name, "nda", 3) == 0 ||
               strncmp(m->name, "nvme", 4) == 0) {
        m->kind = FMON_DISK_SSD;   /* NVMe 一定是 SSD */
    } else {
        int cam = cam_rotation_kind(m->name);

        if (cam >= 0)
            m->kind = (uint32_t)cam;
        else if (strcasestr(m->model, "ssd") != NULL)
            m->kind = FMON_DISK_SSD;
        else if (strcasestr(m->model, "datatraveler") != NULL ||
                 strcasestr(m->model, "cruzer") != NULL ||
                 strcasestr(m->model, "flash") != NULL ||
                 strcasestr(m->model, "usb") != NULL)
            m->kind = FMON_DISK_FLASH;
        else if (m->size >= (uint64_t)256 * 1024 * 1024 * 1024)
            m->kind = FMON_DISK_HDD;   /* 大容量 USB 盘按机械盘处理 */
        else
            m->kind = FMON_DISK_FLASH; /* 小容量 USB 盘按闪存盘处理 */
    }

    /* 接口类型（kind 高 8 位） */
    uint32_t iface;

    if (strncmp(m->name, "nda", 3) == 0 ||
        strncmp(m->name, "nvme", 4) == 0)
        iface = FMON_DISK_IFACE_NVME;
    else if (strncmp(m->name, "ada", 3) == 0)
        iface = FMON_DISK_IFACE_SATA;
    else if (strncmp(m->name, "da", 2) == 0) {
        int spd = usb_storage_speed(fmon_now_ms());

        if (spd > 0) {
            iface = FMON_DISK_IFACE_USB |
                    ((spd == 2) ? FMON_DISK_USB3 : FMON_DISK_USB2);
        } else {
            iface = FMON_DISK_IFACE_SCSI;
        }
    }
    else
        iface = 0;

    if (strncmp(m->name, "nda", 3) == 0 ||
        strncmp(m->name, "nvme", 4) == 0)
        m->kind = (m->kind & ~FMON_DISK_TRANSPORT_MASK) | FMON_DISK_TS_NVME;

    m->kind = (m->kind & (FMON_DISK_TYPE_MASK | FMON_DISK_TRANSPORT_MASK)) |
              iface;
}

/* USB 桥接盘常只报 "Mass Storage Device"：用 root 的 camcontrol identify
 * 读 ATA 的真实型号/序列号/转速。 */
static void cam_enrich(DiskMeta *m, int64_t now)
{
    if (now - m->cam_ms < 60000)
        return;

    m->cam_ms = now;

    char cmd[128];

    snprintf(cmd, sizeof(cmd), "camcontrol identify %s 2>/dev/null", m->name);

    FILE *fp = popen(cmd, "r");

    if (fp == NULL)
        return;

    char line[256];

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;

        while (*p == ' ' || *p == '	')
            p++;

        /* SATA 版本有时是独立一行（不带冒号），先按整行判断 */
        if (strcasestr(p, "SATA 3") != NULL)
            m->kind = (m->kind & ~FMON_DISK_TRANSPORT_MASK) |
                      FMON_DISK_TS_SATA3;
        else if (strcasestr(p, "SATA 2") != NULL)
            m->kind = (m->kind & ~FMON_DISK_TRANSPORT_MASK) |
                      FMON_DISK_TS_SATA2;
        else if (strcasestr(p, "SATA 1") != NULL)
            m->kind = (m->kind & ~FMON_DISK_TRANSPORT_MASK) |
                      FMON_DISK_TS_SATA1;

        char *colon = strchr(p, ':');

        if (colon == NULL)
            continue;

        *colon = '\0';

        char *val = colon + 1;

        while (*val == ' ' || *val == '\t')
            val++;

        char *e = val + strlen(val);

        while (e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            *--e = '\0';

        if (strcasestr(p, "SATA") != NULL) {
            uint32_t ts = 0;

            if (strcasestr(p, "SATA 3") != NULL || strcasestr(val, "SATA 3") != NULL)
                ts = FMON_DISK_TS_SATA3;
            else if (strcasestr(p, "SATA 2") != NULL ||
                     strcasestr(val, "SATA 2") != NULL)
                ts = FMON_DISK_TS_SATA2;
            else if (strcasestr(p, "SATA 1") != NULL ||
                     strcasestr(val, "SATA 1") != NULL)
                ts = FMON_DISK_TS_SATA1;

            if (ts != 0)
                m->kind = (m->kind & ~FMON_DISK_TRANSPORT_MASK) | ts;
        }

        if (strcmp(p, "Model Number") == 0 || strcmp(p, "Model") == 0) {
            if (val[0] != '\0' && strcasestr(val, "mass storage") == NULL)
                strlcpy(m->model, val, sizeof(m->model));
        } else if (strcmp(p, "Serial Number") == 0) {
            if (val[0] != '\0')
                strlcpy(m->serial, val, sizeof(m->serial));
        } else if (strcmp(p, "Rotation Rate") == 0) {
            if (strcasestr(val, "solid") != NULL) {
                m->rpm       = 0;
                m->rpm_known = 1;
            } else if (val[0] >= '0' && val[0] <= '9') {
                m->rpm       = (uint32_t)strtoul(val, NULL, 10);
                m->rpm_known = 1;
            }
        }
    }

    pclose(fp);
}

/* geom disk list <name>：模型/序列号/容量/rotationrate，普通用户可读。
 * rotationrate 是 FreeBSD 判断 SSD/HDD 的标准字段。 */
static int parse_geom(const char *out, DiskMeta *m)
{
    if (out == NULL)
        return 0;

    char *copy = strdup(out);

    if (copy == NULL)
        return 0;

    char *save = NULL;
    int   got  = 0;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        while (*line == ' ' || *line == '\t')
            line++;

        char *colon = strchr(line, ':');

        if (colon == NULL)
            continue;

        *colon = '\0';

        char *val = colon + 1;

        while (*val == ' ' || *val == '\t')
            val++;

        char *tail = val + strlen(val);

        while (tail > val && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = '\0';

        if (strcmp(line, "descr") == 0) {
            strlcpy(m->model, val, sizeof(m->model));
            got = 1;
        } else if (strcmp(line, "ident") == 0) {
            strlcpy(m->serial, val, sizeof(m->serial));
            got = 1;
        } else if (strcmp(line, "Mediasize") == 0) {
            m->size = strtoull(val, NULL, 10);
            got = 1;
        } else if (strcmp(line, "rotationrate") == 0) {
            if (val[0] >= '0' && val[0] <= '9') {
                m->rpm       = (uint32_t)strtoul(val, NULL, 10);
                m->rpm_known = 1;
            } else {
                m->rpm       = 0;
                m->rpm_known = 0;
            }
            got = 1;
        }
    }

    free(copy);
    return got;
}

/* 每条采样跑一次 geom disk list（单进程），热插拔/换盘 1 秒内可见。 */
static void geom_refresh_all(int64_t now)
{
    const char *const argv[] = { "geom", "disk", "list", NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 64 * 1024, 2000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return;
    }

    char *copy = strdup(res.out);

    fmon_child_result_free(&res);

    if (copy == NULL)
        return;

    DiskMeta *cur = NULL;
    DiskMeta *touched[FMON_MAX_DISKS];
    int       nt   = 0;
    char     *save = NULL;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;

        if (strncmp(p, "Geom name:", 10) == 0) {
            char *v = p + 10;

            while (*v == ' ' || *v == '\t')
                v++;

            char *e = v + strlen(v);

            while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';

            cur = meta_slot(v);

            if (cur != NULL && nt < FMON_MAX_DISKS) {
                int seen = 0;

                for (int i = 0; i < nt; i++)
                    if (touched[i] == cur)
                        seen = 1;

                if (!seen)
                    touched[nt++] = cur;
            }

            continue;
        }

        if (cur == NULL)
            continue;

        char *colon = strchr(p, ':');

        if (colon == NULL)
            continue;

        *colon = '\0';

        char *val = colon + 1;

        while (*val == ' ' || *val == '\t')
            val++;

        char *e = val + strlen(val);

        while (e > val && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = '\0';

        if (strcmp(p, "descr") == 0)
            strlcpy(cur->model, val, sizeof(cur->model));
        else if (strcmp(p, "ident") == 0)
            strlcpy(cur->serial, val, sizeof(cur->serial));
        else if (strcmp(p, "Mediasize") == 0)
            cur->size = strtoull(val, NULL, 10);
        else if (strcmp(p, "rotationrate") == 0) {
            if (val[0] >= '0' && val[0] <= '9') {
                cur->rpm       = (uint32_t)strtoul(val, NULL, 10);
                cur->rpm_known = 1;
            } else {
                cur->rpm       = 0;
                cur->rpm_known = 0;
            }
        }
    }

    for (int i = 0; i < nt; i++) {
        DiskMeta *m = touched[i];

        if (m->size != m->last_size) {
            m->last_size = m->size;
            m->cam_ms    = 0;   /* 换盘：允许立即重新执行 camcontrol */
        }

        if (strcasestr(m->model, "mass storage") != NULL ||
            strcasestr(m->model, "usb") != NULL)
            cam_enrich(m, now);

        finalize_kind(m);
        m->updated_ms = now;
    }

    free(copy);
}

static void parse_diskinfo(const char *out, DiskMeta *m)
{
    char *copy = strdup(out);
    char *save = NULL;

    /* diskinfo -v 的格式是 "<值>\t# <说明>"，按说明文字匹配字段 */
    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        char *hash = strchr(line, '#');

        if (hash == NULL)
            continue;

        *hash = '\0';

        char *val = line;

        while (*val == ' ' || *val == '\t')
            val++;

        char *tail = val + strlen(val);

        while (tail > val && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = '\0';

        const char *comment = hash + 1;

        if (strstr(comment, "Disk descr.") != NULL) {
            strlcpy(m->model, val, sizeof(m->model));
        } else if (strstr(comment, "Disk ident.") != NULL) {
            strlcpy(m->serial, val, sizeof(m->serial));
        } else if (strstr(comment, "mediasize in bytes") != NULL) {
            m->size = strtoull(val, NULL, 10);
        } else if (strstr(comment, "Rotation rate") != NULL) {
            if (val[0] >= '0' && val[0] <= '9') {
                m->rpm       = (uint32_t)strtoul(val, NULL, 10);
                m->rpm_known = 1;
            } else {
                /* USB 移动硬盘常报 "Unknown"：不能当成 SSD */
                m->rpm       = 0;
                m->rpm_known = 0;
            }
        }
    }

    free(copy);

    finalize_kind(m);
}

/* 设备被替换（同名 da0 换盘）时让缓存立即失效 */
static void meta_invalidate(const char *name)
{
    for (int i = 0; i < FMON_MAX_DISKS; i++) {
        if (g_meta[i].name[0] != '\0' && strcmp(g_meta[i].name, name) == 0) {
            g_meta[i].updated_ms = 0;
            return;
        }
    }
}

static void fill_disk_meta(FmonDiskInfo *di, int64_t now)
{
    DiskMeta *m = meta_slot(di->name);

    if (m == NULL)
        return;

    if (m->updated_ms == 0 || now - m->updated_ms > 60000) {
        FmonChildResult res;
        int ok = 0;

        const char *const gargv[] = { "geom", "disk", "list", m->name, NULL };

        if (fmon_child_run(gargv, 32 * 1024, 2000, &res) == 0 &&
            fmon_child_ok(&res) && res.out != NULL)
            ok = parse_geom(res.out, m);

        fmon_child_result_free(&res);

        /* geom 不可用时退回 diskinfo */
        if (!ok) {
            char dev[64];

            snprintf(dev, sizeof(dev), "/dev/%s", m->name);

            const char *const dargv[] = { "diskinfo", "-v", dev, NULL };

            if (fmon_child_run(dargv, 32 * 1024, 2000, &res) == 0 &&
                fmon_child_ok(&res) && res.out != NULL)
                parse_diskinfo(res.out, m);

            fmon_child_result_free(&res);
        }

        finalize_kind(m);
        m->updated_ms = now;
    }

    strlcpy(di->model, m->model, sizeof(di->model));
    strlcpy(di->serial, m->serial, sizeof(di->serial));
    di->size_bytes   = m->size;
    di->kind         = m->kind;
    di->rotation_rpm = m->rpm;
}

static void sample_disk(FmonShmData *s)
{
    static bool     primed  = false;
    static int64_t  prev_ms = 0;
    static uint64_t prev_r  = 0, prev_w = 0;

    /* 每块磁盘上一次的累计值（按设备名匹配，防止设备顺序变化） */
    static struct {
        char     name[16];
        uint64_t rb, wb;
        double   busy_us;
        int64_t  ms;
    } dprev[FMON_MAX_DISKS];

    if (g_devstat.dinfo == NULL) {
        g_devstat.dinfo = calloc(1, sizeof(*g_devstat.dinfo));
        if (g_devstat.dinfo == NULL)
            return;
    }

    if (devstat_getdevs(NULL, &g_devstat) < 0 || g_devstat.dinfo->numdevs <= 0)
        return;

    int64_t now = fmon_now_ms();

    /* 热插拔：每采样刷新一次磁盘身份，换盘 / 拔插立即反映 */
    geom_refresh_all(now);

    uint64_t r = 0, w = 0;
    uint64_t ops_r = 0, ops_w = 0;
    int      n = 0;

    for (int i = 0; i < g_devstat.dinfo->numdevs; i++) {
        const struct devstat *d = &g_devstat.dinfo->devices[i];

        /* PASS 设备是原始直通设备，和整盘设备统计的是同一份 I/O */
        if ((d->device_type & DEVSTAT_TYPE_PASS) != 0)
            continue;

        uint64_t rb = d->bytes[DEVSTAT_READ];
        uint64_t wb = d->bytes[DEVSTAT_WRITE];

        r     += rb;
        w     += wb;
        ops_r += d->operations[DEVSTAT_READ];
        ops_w += d->operations[DEVSTAT_WRITE];

        /* 每块磁盘的明细（最多 FMON_MAX_DISKS 块） */
        if (n < FMON_MAX_DISKS) {
            FmonDiskInfo *di = &s->disks[n];

            memset(di, 0, sizeof(*di));

            char mapped[32];

            map_disk_name(d->device_name, mapped, sizeof(mapped));
            strlcpy(di->name, mapped, sizeof(di->name));

            /* 同名设备累计字节突然变小：说明换了盘，重新探测型号/类型 */
            if (dprev[n].ms > 0 &&
                (rb < dprev[n].rb || wb < dprev[n].wb))
                meta_invalidate(mapped);

            fill_disk_meta(di, now);
            di->read_bytes  = rb;
            di->write_bytes = wb;
            di->read_ops    = d->operations[DEVSTAT_READ];
            di->write_ops   = d->operations[DEVSTAT_WRITE];

            if (strcmp(dprev[n].name, d->device_name) != 0) {
                strlcpy(dprev[n].name, d->device_name, sizeof(dprev[n].name));
                dprev[n].ms = 0;
            }

            if (dprev[n].ms > 0 && now > dprev[n].ms) {
                double dt = (double)(now - dprev[n].ms) / 1000.0;

                if (dt > 0.05) {
                    if (rb >= dprev[n].rb)
                        di->read_kbps =
                            (double)(rb - dprev[n].rb) / 1024.0 / dt;
                    if (wb >= dprev[n].wb)
                        di->write_kbps =
                            (double)(wb - dprev[n].wb) / 1024.0 / dt;
                    if (d->busy_time.sec >= 0) {
                        double busy_us =
                            (double)d->busy_time.sec * 1000000.0 +
                            (double)d->busy_time.frac * 1000000.0 / 4294967296.0;

                        if (busy_us >= dprev[n].busy_us)
                            di->busy_pct =
                                100.0 * (busy_us - dprev[n].busy_us) /
                                1000000.0 / dt;
                        if (di->busy_pct > 100.0)
                            di->busy_pct = 100.0;
                    }
                }
            }

            dprev[n].rb   = rb;
            dprev[n].wb   = wb;
            dprev[n].busy_us =
                (double)d->busy_time.sec * 1000000.0 +
                (double)d->busy_time.frac * 1000000.0 / 4294967296.0;
            dprev[n].ms   = now;
            n++;
        }
    }

    s->disk_count = (uint32_t)n;

    if (primed && now > prev_ms) {
        double dt = (double)(now - prev_ms) / 1000.0;
        if (dt > 0.05) {
            if (r >= prev_r)
                s->disk_read_kbps = (double)(r - prev_r) / 1024.0 / dt;
            if (w >= prev_w)
                s->disk_write_kbps = (double)(w - prev_w) / 1024.0 / dt;
        }
    }

    s->disk_read_ops  = ops_r;
    s->disk_write_ops = ops_w;

    prev_r  = r;
    prev_w  = w;
    prev_ms = now;
    primed  = true;
}

/* ===========================================================================
 * 线程主循环
 * ========================================================================= */
void *thread_io(void *arg)
{
    (void)arg;

    int tick = 0;

    while (g_running) {
        /* devstat 是纯 sysctl 读取，微秒级，放在写事务里没有问题 */
        fmon_shm_wr_begin();
        sample_disk(g_shm);
        fmon_shm_wr_end();

        /* GPU 探测较重（可能要 fork nvidia-smi，数百毫秒），
         * 由 fmon_gpu_collect() 自己管理写锁，避免阻塞发布线程的心跳。 */
        if ((tick % 5) == 0)
            fmon_gpu_collect(g_shm);

        tick++;

        if (!fmon_sleep_ms(FMON_SAMPLE_INTERVAL_MS) && !g_running)
            break;
    }

    devstat_release();
    return NULL;
}














