/*
 * collector_io_meta.c - 磁盘型号/容量/CAM 元数据缓存
 */

#include "collector_io_meta.h"

#include "disk_meta.h"
#include "fmon_child.h"
#include "fmon_sysctl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* 磁盘型号/容量/类型：每块盘每 60 秒查一次 diskinfo，结果缓存 */
typedef struct {
    char     name[16];
    char     model[64];
    char     serial[32];
    uint64_t size;
    uint32_t rpm;
    int      rpm_known;   /* diskinfo 的 Rotation rate 是否为确定值 */
    int      meta_valid;  /* 内核/工具至少拿到一项元数据 */
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
    const char *const argv[] = { "camcontrol", "identify", name, NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 64 * 1024, 2000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return -1;
    }

    int   kind = -1;
    char *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
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

    fmon_child_result_free(&res);
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

    char driver[16];
    size_t n = 0;

    while (name[n] != '\0' && (name[n] < '0' || name[n] > '9') &&
           n + 1 < sizeof(driver)) {
        driver[n] = name[n];
        n++;
    }
    driver[n] = '\0';

    char oid[64];

    snprintf(oid, sizeof(oid), "kern.cam.%s.%d.rotating", driver,
             (int)strtol(d, NULL, 10));

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

    const char *const argv[] = { "usbconfig", "list", NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 64 * 1024, 2000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return speed;
    }

    char *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
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

        /* 用接口类判断是不是大容量存储：bInterfaceClass = 0x08。
         * 参数通过 argv 传递，设备名不会被 shell 解释。 */
        const char *const dargv[] = { "usbconfig", "-d", dev,
                                      "dump_curr_config_desc", NULL };
        FmonChildResult dres;
        int storage = 0;

        if (fmon_child_run(dargv, 64 * 1024, 2000, &dres) == 0 &&
            fmon_child_ok(&dres) && dres.out != NULL) {
            if (strcasestr(dres.out, "bInterfaceClass = 0x08") != NULL ||
                strcasestr(dres.out, "bInterfaceClass = 8") != NULL ||
                strcasestr(dres.out, "Mass Storage") != NULL)
                storage = 1;
        }

        fmon_child_result_free(&dres);

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

    fmon_child_result_free(&res);
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

        /* 没有内核/标准命令确认旋转介质时保持 UNKNOWN。
         * 不再根据型号字符串或容量猜测 SSD/HDD/闪存。 */
        m->kind = (cam >= 0) ? (uint32_t)cam : FMON_DISK_UNKNOWN;
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

    const char *const argv[] = { "camcontrol", "identify", m->name, NULL };
    FmonChildResult res;

    if (fmon_child_run(argv, 128 * 1024, 2500, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return;
    }

    char *save = NULL;

    for (char *line = strtok_r(res.out, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = line;

        while (*p == ' ' || *p == '\t')
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

    fmon_child_result_free(&res);
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
void disk_io_meta_invalidate(const char *name)
{
    for (int i = 0; i < FMON_MAX_DISKS; i++) {
        if (g_meta[i].name[0] != '\0' && strcmp(g_meta[i].name, name) == 0) {
            g_meta[i].updated_ms = 0;
            return;
        }
    }
}

void disk_io_fill_meta(FmonDiskInfo *di, int64_t now)
{
    DiskMeta *m = meta_slot(di->name);

    if (m == NULL)
        return;

    if (m->updated_ms == 0 || now - m->updated_ms > 60000) {
        FmonDiskMeta km;

        m->meta_valid = 0;

        /* 第一优先级：内核 ioctl / NVMe controller / CAM ATA/SCSI。 */
        if (fmon_disk_meta_probe(m->name, &km)) {
            strlcpy(m->model, km.model, sizeof(m->model));
            strlcpy(m->serial, km.serial, sizeof(m->serial));
            m->size = km.size_bytes;
            m->kind = km.kind;
            m->rpm  = km.rotation_rpm;
            m->rpm_known =
                (km.rotation_rpm > 0 ||
                 (km.kind & FMON_DISK_TYPE_MASK) == FMON_DISK_SSD) ? 1 : 0;
            m->meta_valid = 1;
        } else {
            /* 回退：geom / diskinfo 都是 base 工具，失败时保持 UNKNOWN。 */
            FmonChildResult res;
            int ok = 0;

            const char *const gargv[] = {
                "geom", "disk", "list", m->name, NULL
            };

            if (fmon_child_run(gargv, 32 * 1024, 2000, &res) == 0 &&
                fmon_child_ok(&res) && res.out != NULL)
                ok = parse_geom(res.out, m);

            fmon_child_result_free(&res);

            if (!ok) {
                char dev[64];

                snprintf(dev, sizeof(dev), "/dev/%s", m->name);

                const char *const dargv[] = { "diskinfo", "-v", dev, NULL };

                if (fmon_child_run(dargv, 32 * 1024, 2000, &res) == 0 &&
                    fmon_child_ok(&res) && res.out != NULL)
                    parse_diskinfo(res.out, m);

                fmon_child_result_free(&res);
            }

            if (strcasestr(m->model, "mass storage") != NULL ||
                strcasestr(m->model, "usb") != NULL)
                cam_enrich(m, now);

            finalize_kind(m);
            m->meta_valid = (m->model[0] != '\0' || m->serial[0] != '\0' ||
                             m->size > 0) ? 1 : 0;
        }

        if (m->size != m->last_size) {
            m->last_size = m->size;
            m->cam_ms    = 0;
        }

        m->updated_ms = now;
    }

    strlcpy(di->model, m->model, sizeof(di->model));
    strlcpy(di->serial, m->serial, sizeof(di->serial));
    di->size_bytes   = m->size;
    di->kind         = m->kind;
    di->rotation_rpm = m->rpm;
    di->meta_valid   = (m->meta_valid != 0) ? 1u : 0u;
}
