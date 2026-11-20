/*
 * disk_smart.c - 直接用 FreeBSD 内核接口读取磁盘温度
 *
 * 覆盖：
 *   - NVMe：NVME_GET_NSID + NVME_PASSTHROUGH_CMD(GET_LOG_PAGE 0x02)；
 *   - ATA/SATA：libcam 的 CAM ATAIO + SMART READ DATA（0xB0/0xD0）；
 *   - SCSI/SAS：libcam 的 CAM SCSI LOG SENSE page 0x0D。
 *
 * 失败时 info->valid = false，上层显示 “—”，不伪造温度。
 */

#include "disk_smart.h"

#include "fmon_child.h"
#include "fmon_shm.h"

#include <sys/types.h>
#include <sys/param.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/ata/ata_all.h>
#include <cam/scsi/scsi_all.h>

#include <dev/nvme/nvme.h>
#include <sys/ata.h>
#include <sys/endian.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libcam 的公开 API；FreeBSD base 提供 libcam，但没有安装 camlib.h。 */
struct cam_device;

struct cam_device *cam_open_spec_device(const char *dev_name, int unit,
                                        int flags, struct cam_device *device);
union ccb         *cam_getccb(struct cam_device *dev);
void               cam_freeccb(union ccb *ccb);
int                cam_send_ccb(struct cam_device *device, union ccb *ccb);
void               cam_close_device(struct cam_device *dev);

#define DISK_SMART_CACHE_MAX FMON_MAX_DISKS

typedef struct {
    char          name[16];
    bool          tried;
    int64_t       ms;
    uint32_t      source;
    FmonSmartInfo info;
} DiskSmartCache;

static DiskSmartCache g_cache[DISK_SMART_CACHE_MAX];

static DiskSmartCache *cache_slot(const char *name)
{
    for (size_t i = 0; i < DISK_SMART_CACHE_MAX; i++) {
        if (g_cache[i].name[0] != '\0' && strcmp(g_cache[i].name, name) == 0)
            return &g_cache[i];
    }

    for (size_t i = 0; i < DISK_SMART_CACHE_MAX; i++) {
        if (g_cache[i].name[0] == '\0') {
            strlcpy(g_cache[i].name, name, sizeof(g_cache[i].name));
            return &g_cache[i];
        }
    }

    return NULL;
}

static int unit_number(const char *name)
{
    const char *p = name;

    while (*p != '\0' && (*p < '0' || *p > '9'))
        p++;

    return (*p != '\0') ? (int)strtol(p, NULL, 10) : -1;
}

/* -------------------------------------------------------------------------
 * NVMe：直接 ioctl
 * ------------------------------------------------------------------------- */
/* 从 ndaN 反查控制器路径；不假设 ndaN → nvmeN。 */
bool fmon_nvme_controller_path(const char *name, char *out, size_t out_len)
{
    char disk_path[64];

    snprintf(disk_path, sizeof(disk_path), "/dev/%s", name);

    if (strncmp(name, "nvme", 4) == 0) {
        strlcpy(out, disk_path, out_len);
        return true;
    }

    int fd = open(disk_path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;

    struct nvme_get_nsid gnsid;

    memset(&gnsid, 0, sizeof(gnsid));

    bool ok = ioctl(fd, NVME_GET_NSID, &gnsid) == 0 && gnsid.cdev[0] != '\0';

    close(fd);

    if (!ok)
        return false;

    if (gnsid.cdev[0] == '/')
        strlcpy(out, gnsid.cdev, out_len);
    else
        snprintf(out, out_len, "/dev/%s", gnsid.cdev);

    return out[0] != '\0';
}

static bool nvme_probe(const char *name, FmonSmartInfo *out)
{
    char ctrl_path[SPECNAMELEN + 16];

    if (!fmon_nvme_controller_path(name, ctrl_path, sizeof(ctrl_path)))
        return false;

    int fd = open(ctrl_path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;

    struct nvme_health_information_page health;
    struct nvme_pt_command             pt;
    uint32_t numd = sizeof(health) / sizeof(uint32_t) - 1;

    memset(&health, 0, sizeof(health));
    memset(&pt, 0, sizeof(pt));

    pt.cmd.opc  = NVME_OPC_GET_LOG_PAGE;
    pt.cmd.nsid = htole32(NVME_GLOBAL_NAMESPACE_TAG);
    pt.cmd.cdw10 = htole32((numd << 16) | NVME_LOG_HEALTH_INFORMATION);
    pt.cmd.cdw11 = htole32(numd >> 16);
    pt.buf       = &health;
    pt.len       = (uint32_t)sizeof(health);
    pt.is_read   = 1;

    int rc = ioctl(fd, NVME_PASSTHROUGH_CMD, &pt);

    close(fd);

    if (rc < 0 || nvme_completion_is_error(&pt.cpl))
        return false;

    return fmon_smart_parse_nvme_health((const uint8_t *)&health,
                                        sizeof(health), out);
}

/* -------------------------------------------------------------------------
 * libcam 公共辅助
 * ------------------------------------------------------------------------- */
static union ccb *cam_prepare(const char *driver, int unit,
                              struct cam_device **dev_out)
{
    int unit_num = unit;

    if (unit_num < 0)
        return NULL;

    struct cam_device *dev =
        cam_open_spec_device(driver, unit_num, O_RDWR, NULL);

    if (dev == NULL)
        return NULL;

    union ccb *ccb = cam_getccb(dev);

    if (ccb == NULL) {
        cam_close_device(dev);
        return NULL;
    }

    *dev_out = dev;
    return ccb;
}

static bool cam_finish(struct cam_device *dev, union ccb *ccb)
{
    bool ok = (ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP;

    cam_freeccb(ccb);
    cam_close_device(dev);
    return ok;
}

/* -------------------------------------------------------------------------
 * ATA/SATA：CAM ATAIO + SMART READ DATA
 * ------------------------------------------------------------------------- */
static bool ata_probe(const char *name, FmonSmartInfo *out)
{
    struct cam_device *dev = NULL;
    union ccb         *ccb = cam_prepare("ada", unit_number(name), &dev);

    if (ccb == NULL)
        return false;

    uint8_t data[512];

    memset(data, 0, sizeof(data));
    CCB_CLEAR_ALL_EXCEPT_HDR(&ccb->ataio);
    cam_fill_ataio(&ccb->ataio, 1, NULL, CAM_DIR_IN, 0, data,
                   (uint32_t)sizeof(data), 5000);

    /* SMART READ DATA: command 0xB0, feature 0xD0, LBA 0xC24F, 1 sector. */
    ata_28bit_cmd(&ccb->ataio, ATA_SMART_CMD, 0xd0, 0x00c24f, 1);
    ccb->ccb_h.flags |= CAM_DEV_QFRZDIS;

    bool sent = cam_send_ccb(dev, ccb) == 0;

    if (!sent) {
        cam_freeccb(ccb);
        cam_close_device(dev);
        return false;
    }

    bool ok = cam_finish(dev, ccb);

    if (!ok)
        return false;

    return fmon_smart_parse_ata_smart(data, sizeof(data), out);
}

/* -------------------------------------------------------------------------
 * SCSI/SAS：CAM SCSI LOG SENSE page 0x0D
 * ------------------------------------------------------------------------- */
static bool scsi_probe(const char *name, FmonSmartInfo *out)
{
    struct cam_device *dev = NULL;
    union ccb         *ccb = cam_prepare("da", unit_number(name), &dev);

    if (ccb == NULL)
        return false;

    uint8_t data[252];
    uint8_t sense[32];

    memset(data, 0, sizeof(data));
    memset(sense, 0, sizeof(sense));

    struct scsi_log_sense cdb;

    memset(&cdb, 0, sizeof(cdb));
    cdb.opcode    = 0x4d;             /* LOG SENSE */
    cdb.page      = SLS_TEMPERATURE;  /* 0x0D */
    cdb.length[0] = (uint8_t)(sizeof(data) >> 8);
    cdb.length[1] = (uint8_t)(sizeof(data) & 0xff);

    CCB_CLEAR_ALL_EXCEPT_HDR(&ccb->csio);
    cam_fill_csio(&ccb->csio, 1, NULL, CAM_DIR_IN, 0, data,
                  (uint32_t)sizeof(data), (uint8_t)sizeof(sense),
                  (uint8_t)sizeof(cdb), 5000);
    memcpy(ccb->csio.cdb_io.cdb_bytes, &cdb, sizeof(cdb));
    ccb->ccb_h.flags |= CAM_DEV_QFRZDIS;

    bool sent = cam_send_ccb(dev, ccb) == 0;

    if (!sent) {
        cam_freeccb(ccb);
        cam_close_device(dev);
        return false;
    }

    bool    ok    = (ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP;
    uint8_t status = ccb->csio.scsi_status;

    cam_freeccb(ccb);
    cam_close_device(dev);

    if (!ok || status != 0)
        return false;

    return fmon_smart_parse_scsi_temperature(data, sizeof(data), out);
}

/* -------------------------------------------------------------------------
 * 基础工具 / 可选工具回退
 *
 * 只在 kernel provider 失败时使用；工具路径从 PATH 查找，不写死 /dev 或
 * 控制器编号。命令通过 fmon_child_run() 的 argv 执行，不经过 shell。
 * ------------------------------------------------------------------------- */
static bool find_tool(const char *name, char *out, size_t out_len)
{
    const char *path = getenv("PATH");

    if (path == NULL || name == NULL || out == NULL || out_len == 0)
        return false;

    while (*path != '\0') {
        const char *sep = strchr(path, ':');
        size_t      len = (sep != NULL) ? (size_t)(sep - path) : strlen(path);

        if (len > 0 && len < 400) {
            char candidate[512];

            snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)len, path,
                     name);

            if (access(candidate, X_OK) == 0) {
                strlcpy(out, candidate, out_len);
                return true;
            }
        }

        if (sep == NULL)
            break;
        path = sep + 1;
    }

    return false;
}

static bool run_tool_text(const char *path, const char *const argv[],
                          char **out)
{
    (void)path;

    FmonChildResult res;

    *out = NULL;

    if (fmon_child_run(argv, 128 * 1024, 2500, &res) != 0 ||
        res.out == NULL) {
        fmon_child_result_free(&res);
        return false;
    }

    *out = res.out;
    res.out = NULL;
    fmon_child_result_free(&res);
    return true;
}

static bool nvmecontrol_probe(const char *name, FmonSmartInfo *out)
{
    char tool[512];

    if (!find_tool("nvmecontrol", tool, sizeof(tool)))
        return false;

    char dev[64];

    snprintf(dev, sizeof(dev), "/dev/%s", name);

    const char *const argv[] = { tool, "logpage", "-p", "2", dev, NULL };
    char *text = NULL;

    if (!run_tool_text(tool, argv, &text))
        return false;

    bool ok = fmon_smart_parse_nvmecontrol(text, out);

    free(text);
    return ok;
}

static bool smartctl_probe(const char *name, FmonSmartInfo *out)
{
    char tool[512];

    if (!find_tool("smartctl", tool, sizeof(tool)))
        return false;

    char dev[SPECNAMELEN + 16];

    bool nvme = strncmp(name, "nvme", 4) == 0 ||
                strncmp(name, "nda", 3) == 0;

    if (nvme) {
        if (!fmon_nvme_controller_path(name, dev, sizeof(dev)))
            return false;
    } else {
        snprintf(dev, sizeof(dev), "/dev/%s", name);
    }

    const char *const argv_ata[] = { tool, "-A", "-n", "standby", dev, NULL };
    const char *const argv_nvme[] = { tool, "-A", dev, NULL };
    char *text = NULL;

    if (!run_tool_text(tool, nvme ? argv_nvme : argv_ata, &text))
        return false;

    bool ok = fmon_smart_parse_smartctl(text, out);

    free(text);
    return ok;
}

/* -------------------------------------------------------------------------
 * 入口 + 30 秒缓存
 * ------------------------------------------------------------------------- */
void fmon_disk_smart_probe(const char *disk_name, int64_t now_ms,
                           FmonSmartInfo *out, uint32_t *source)
{
    if (out == NULL)
        return;

    out->valid     = false;
    out->temp_c    = -1.0;
    out->spare_pct = -1;
    out->wear_pct  = -1;

    if (source != NULL)
        *source = FMON_SMART_SRC_NONE;

    if (disk_name == NULL || disk_name[0] == '\0')
        return;

    DiskSmartCache *slot = cache_slot(disk_name);

    if (slot == NULL)
        return;

    if (!slot->tried || now_ms - slot->ms >= 30000) {
        FmonSmartInfo info;
        uint32_t      src = FMON_SMART_SRC_NONE;
        bool          ok  = false;

        memset(&info, 0, sizeof(info));

        if (strncmp(disk_name, "nvme", 4) == 0 ||
            strncmp(disk_name, "nda", 3) == 0) {
            if (nvme_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_NVME_IOCTL;
                ok  = true;
            } else if (nvmecontrol_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_NVMECONTROL;
                ok  = true;
            } else if (smartctl_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_SMARTCTL;
                ok  = true;
            }
        } else if (strncmp(disk_name, "ada", 3) == 0) {
            if (ata_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_ATA_CAM;
                ok  = true;
            } else if (smartctl_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_SMARTCTL;
                ok  = true;
            }
        } else if (strncmp(disk_name, "da", 2) == 0) {
            if (scsi_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_SCSI_CAM;
                ok  = true;
            } else if (smartctl_probe(disk_name, &info)) {
                src = FMON_SMART_SRC_SMARTCTL;
                ok  = true;
            }
        }

        slot->tried  = true;
        slot->ms     = now_ms;
        slot->source = ok ? src : FMON_SMART_SRC_NONE;

        if (ok)
            slot->info = info;
        else
            memset(&slot->info, 0, sizeof(slot->info));
    }

    *out = slot->info;
    if (source != NULL)
        *source = slot->source;
}
