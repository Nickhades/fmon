/*
 * disk_meta.c - 直接用 FreeBSD 内核接口读取磁盘型号/序列号/容量/转速
 */

#include "disk_meta.h"
#include "disk_smart.h"
#include "fmon_shm.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <sys/ata.h>
#include <sys/sysctl.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/ata/ata_all.h>
#include <cam/scsi/scsi_all.h>

#include <dev/nvme/nvme.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libcam 公共 API；FreeBSD base 提供 libcam，但没有安装 camlib.h。 */
struct cam_device;

struct cam_device *cam_open_spec_device(const char *dev_name, int unit,
                                        int flags, struct cam_device *device);
union ccb         *cam_getccb(struct cam_device *dev);
void               cam_freeccb(union ccb *ccb);
int                cam_send_ccb(struct cam_device *device, union ccb *ccb);
void               cam_close_device(struct cam_device *dev);

static void trim_ascii(char *s)
{
    if (s == NULL)
        return;

    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static int unit_number(const char *name)
{
    const char *p = name;

    while (*p != '\0' && (*p < '0' || *p > '9'))
        p++;

    return (*p != '\0') ? (int)strtol(p, NULL, 10) : -1;
}

static void driver_name(const char *name, char *out, size_t out_len)
{
    size_t n = 0;

    while (name[n] != '\0' && (name[n] < '0' || name[n] > '9') &&
           n + 1 < out_len) {
        out[n] = name[n];
        n++;
    }

    out[n] = '\0';
}

static int cam_rotating(const char *name)
{
    char driver[16];
    int  unit = unit_number(name);

    if (unit < 0)
        return -1;

    driver_name(name, driver, sizeof(driver));

    char oid[64];

    snprintf(oid, sizeof(oid), "kern.cam.%s.%d.rotating", driver, unit);

    int    v = 0;
    size_t l = sizeof(v);

    if (sysctlbyname(oid, &v, &l, NULL, 0) != 0)
        return -1;

    return v;
}

static bool disk_ioctl_info(const char *name, FmonDiskMeta *out)
{
    char path[64];

    snprintf(path, sizeof(path), "/dev/%s", name);

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;

    off_t media = 0;

    if (ioctl(fd, DIOCGMEDIASIZE, &media) == 0 && media > 0)
        out->size_bytes = (uint64_t)media;

    char ident[DISK_IDENT_SIZE];

    memset(ident, 0, sizeof(ident));

    if (ioctl(fd, DIOCGIDENT, ident) == 0 && ident[0] != '\0') {
        trim_ascii(ident);
        if (ident[0] != '\0')
            strlcpy(out->serial, ident, sizeof(out->serial));
    }

    close(fd);
    return out->size_bytes > 0 || out->serial[0] != '\0';
}

static bool nvme_meta(const char *name, FmonDiskMeta *out)
{
    char ctrl[SPECNAMELEN + 16];

    if (!fmon_nvme_controller_path(name, ctrl, sizeof(ctrl)))
        return false;

    int fd = open(ctrl, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;

    struct nvme_controller_data cdata;
    int rc = ioctl(fd, NVME_GET_CONTROLLER_DATA, &cdata);

    close(fd);

    if (rc < 0)
        return false;

    char model[NVME_MODEL_NUMBER_LENGTH + 1];
    char serial[NVME_SERIAL_NUMBER_LENGTH + 1];

    memcpy(model, cdata.mn, NVME_MODEL_NUMBER_LENGTH);
    model[NVME_MODEL_NUMBER_LENGTH] = '\0';
    memcpy(serial, cdata.sn, NVME_SERIAL_NUMBER_LENGTH);
    serial[NVME_SERIAL_NUMBER_LENGTH] = '\0';

    trim_ascii(model);
    trim_ascii(serial);

    if (model[0] != '\0')
        strlcpy(out->model, model, sizeof(out->model));
    if (serial[0] != '\0')
        strlcpy(out->serial, serial, sizeof(out->serial));

    out->kind         = (FMON_DISK_SSD & FMON_DISK_TYPE_MASK) |
                        FMON_DISK_TS_NVME | FMON_DISK_IFACE_NVME;
    out->rotation_rpm = 0;
    return out->model[0] != '\0' || out->serial[0] != '\0';
}

static bool ata_meta(const char *name, FmonDiskMeta *out)
{
    int unit = unit_number(name);

    if (unit < 0)
        return false;

    struct cam_device *dev = cam_open_spec_device("ada", unit, O_RDWR, NULL);

    if (dev == NULL)
        return false;

    union ccb *ccb = cam_getccb(dev);

    if (ccb == NULL) {
        cam_close_device(dev);
        return false;
    }

    uint8_t data[512];

    memset(data, 0, sizeof(data));
    CCB_CLEAR_ALL_EXCEPT_HDR(&ccb->ataio);
    cam_fill_ataio(&ccb->ataio, 1, NULL, CAM_DIR_IN, 0, data,
                   (uint32_t)sizeof(data), 5000);
    /* ATA IDENTIFY DEVICE */
    ata_28bit_cmd(&ccb->ataio, 0xec, 0, 0, 1);
    ccb->ccb_h.flags |= CAM_DEV_QFRZDIS;

    bool sent = cam_send_ccb(dev, ccb) == 0;
    bool ok   = sent && (ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP;

    cam_freeccb(ccb);
    cam_close_device(dev);

    if (!ok)
        return false;

    struct ata_params *ap = (struct ata_params *)data;

    ata_param_fixup(ap);

    if (ap->model[0] != '\0')
        strlcpy(out->model, (const char *)ap->model, sizeof(out->model));
    if (ap->serial[0] != '\0')
        strlcpy(out->serial, (const char *)ap->serial, sizeof(out->serial));

    uint16_t rr = ap->media_rotation_rate;

    if (rr == 1) {
        out->rotation_rpm = 0;
        out->kind = (FMON_DISK_SSD & FMON_DISK_TYPE_MASK) |
                    FMON_DISK_IFACE_SATA;
    } else if (rr > 1) {
        out->rotation_rpm = rr;
        out->kind = (FMON_DISK_HDD & FMON_DISK_TYPE_MASK) |
                    FMON_DISK_IFACE_SATA;
    } else {
        out->kind = (FMON_DISK_UNKNOWN & FMON_DISK_TYPE_MASK) |
                    FMON_DISK_IFACE_SATA;
    }

    trim_ascii(out->model);
    trim_ascii(out->serial);
    return out->model[0] != '\0' || out->serial[0] != '\0';
}

static bool scsi_meta(const char *name, FmonDiskMeta *out)
{
    int unit = unit_number(name);

    if (unit < 0)
        return false;

    struct cam_device *dev = cam_open_spec_device("da", unit, O_RDWR, NULL);

    if (dev == NULL)
        return false;

    union ccb *ccb = cam_getccb(dev);

    if (ccb == NULL) {
        cam_close_device(dev);
        return false;
    }

    uint8_t data[96];
    uint8_t sense[32];

    memset(data, 0, sizeof(data));
    memset(sense, 0, sizeof(sense));

    uint8_t cdb[6] = { 0x12, 0, 0, 0, sizeof(data), 0 };

    CCB_CLEAR_ALL_EXCEPT_HDR(&ccb->csio);
    cam_fill_csio(&ccb->csio, 1, NULL, CAM_DIR_IN, 0, data,
                  (uint32_t)sizeof(data), (uint8_t)sizeof(sense),
                  (uint8_t)sizeof(cdb), 5000);
    memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, sizeof(cdb));
    ccb->ccb_h.flags |= CAM_DEV_QFRZDIS;

    bool sent = cam_send_ccb(dev, ccb) == 0;
    bool ok   = sent && (ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP &&
                ccb->csio.scsi_status == 0;

    cam_freeccb(ccb);
    cam_close_device(dev);

    if (!ok)
        return false;

    char vendor[9];
    char product[17];

    memcpy(vendor, data + 8, 8);
    vendor[8] = '\0';
    memcpy(product, data + 16, 16);
    product[16] = '\0';
    trim_ascii(vendor);
    trim_ascii(product);

    snprintf(out->model, sizeof(out->model), "%s%s%s", vendor,
             (vendor[0] != '\0' && product[0] != '\0') ? " " : "", product);

    int rot = cam_rotating(name);

    if (rot == 1) {
        out->rotation_rpm = 0;
        out->kind = (FMON_DISK_HDD & FMON_DISK_TYPE_MASK) |
                    FMON_DISK_IFACE_SCSI;
    } else if (rot == 0) {
        out->rotation_rpm = 0;
        out->kind = (FMON_DISK_SSD & FMON_DISK_TYPE_MASK) |
                    FMON_DISK_IFACE_SCSI;
    } else {
        out->kind = (FMON_DISK_UNKNOWN & FMON_DISK_TYPE_MASK) |
                    FMON_DISK_IFACE_SCSI;
    }

    return out->model[0] != '\0';
}

bool fmon_disk_meta_probe(const char *name, FmonDiskMeta *out)
{
    if (out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    if (name == NULL || name[0] == '\0')
        return false;

    bool got = disk_ioctl_info(name, out);

    if (strncmp(name, "nda", 3) == 0 || strncmp(name, "nvme", 4) == 0) {
        got = nvme_meta(name, out) || got;
        if (out->kind == 0)
            out->kind = (FMON_DISK_SSD & FMON_DISK_TYPE_MASK) |
                        FMON_DISK_TS_NVME | FMON_DISK_IFACE_NVME;
    } else if (strncmp(name, "ada", 3) == 0) {
        got = ata_meta(name, out) || got;
        if (out->kind == 0)
            out->kind = FMON_DISK_UNKNOWN | FMON_DISK_IFACE_SATA;
    } else if (strncmp(name, "da", 2) == 0) {
        got = scsi_meta(name, out) || got;
        if (out->kind == 0)
            out->kind = FMON_DISK_UNKNOWN | FMON_DISK_IFACE_SCSI;
    } else {
        int rot = cam_rotating(name);

        if (rot == 1)
            out->kind = (FMON_DISK_HDD & FMON_DISK_TYPE_MASK) |
                        FMON_DISK_IFACE_SCSI;
        else if (rot == 0)
            out->kind = (FMON_DISK_SSD & FMON_DISK_TYPE_MASK) |
                        FMON_DISK_IFACE_SCSI;
        else
            out->kind = FMON_DISK_UNKNOWN;
    }

    out->valid = got || out->size_bytes > 0 || out->model[0] != '\0' ||
                 out->serial[0] != '\0';

    return out->valid;
}
