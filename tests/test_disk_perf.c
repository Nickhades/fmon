/*
 * test_disk_perf.c - 磁盘 SMART 原始页解析
 *
 * 输入是内核 ioctl/CAM 返回的二进制结构，不是外部工具文本：
 *   - NVMe SMART/Health Information (512 bytes)
 *   - ATA SMART READ DATA (512 bytes)
 *   - SCSI LOG SENSE temperature page (0x0D)
 */

#include <glib.h>

#include "fmon_smart.h"

static void test_nvme_health(void)
{
    uint8_t page[512] = { 0 };

    /* struct nvme_health_information_page */
    page[0] = 0;                       /* critical warning */
    page[1] = 315 & 0xff;              /* temperature lo (K) */
    page[2] = (315 >> 8) & 0xff;       /* temperature hi */
    page[3] = 100;                     /* available spare */
    page[4] = 10;                      /* threshold */
    page[5] = 6;                       /* percentage used */

    FmonSmartInfo info;

    g_assert_true(fmon_smart_parse_nvme_health(page, sizeof(page), &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 41.85, 1e-6);
    g_assert_cmpint(info.spare_pct, ==, 100);
    g_assert_cmpint(info.wear_pct, ==, 6);
}

static void test_ata_smart(void)
{
    uint8_t page[512] = { 0 };

    /* attribute table starts at offset 2, 30 entries of 12 bytes */
    page[2] = 194;      /* Temperature_Celsius */
    page[3] = 0;
    page[4] = 0;
    page[5] = 100;      /* value */
    page[6] = 100;      /* worst */
    page[7] = 35;       /* raw[0] = 35 C */

    FmonSmartInfo info;

    g_assert_true(fmon_smart_parse_ata_smart(page, sizeof(page), &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 35.0, 1e-6);

    page[2] = 190;      /* Airflow_Temperature_Cel fallback */
    page[7] = 41;
    g_assert_true(fmon_smart_parse_ata_smart(page, sizeof(page), &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 41.0, 1e-6);
}

static void test_scsi_temperature(void)
{
    uint8_t page[252] = { 0 };

    /* LOG SENSE header: page 0x0D, page length 8 (big endian) */
    page[0] = 0x0d;
    page[2] = 0x00;
    page[3] = 0x08;

    /* parameter 0: code 0x0000, control 0, length 2, reserved, temp */
    page[4] = 0x00;
    page[5] = 0x00;
    page[6] = 0x00;
    page[7] = 0x02;
    page[8] = 0x00;
    page[9] = 37;

    FmonSmartInfo info;

    g_assert_true(fmon_smart_parse_scsi_temperature(page, sizeof(page), &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 37.0, 1e-6);
}

static void test_nvmecontrol_text(void)
{
    const char *text =
        "SMART/Health Information Log\n"
        "Temperature:                    315 K, 41.85 C, 107.33 F\n"
        "Available spare:                100\n"
        "Percentage used:                6\n";

    FmonSmartInfo info;

    g_assert_true(fmon_smart_parse_nvmecontrol(text, &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 41.85, 1e-6);
    g_assert_cmpint(info.spare_pct, ==, 100);
    g_assert_cmpint(info.wear_pct, ==, 6);
}

static void test_smartctl_text(void)
{
    FmonSmartInfo info;

    const char *ata =
        "ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE UPDATED "
        "WHEN_FAILED RAW_VALUE\n"
        "194 Temperature_Celsius     0x0022   100   100   000    Old_age   "
        "Always       -       35 (Min/Max 20/45)\n";
    g_assert_true(fmon_smart_parse_smartctl(ata, &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 35.0, 1e-6);

    const char *nvme =
        "Temperature:                        41 Celsius\n"
        "Available Spare:                    100%\n"
        "Percentage Used:                    6%\n";
    g_assert_true(fmon_smart_parse_smartctl(nvme, &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 41.0, 1e-6);
    g_assert_cmpint(info.spare_pct, ==, 100);

    const char *scsi = "Current Drive Temperature:     37 C\n";
    g_assert_true(fmon_smart_parse_smartctl(scsi, &info));
    g_assert_cmpfloat_with_epsilon(info.temp_c, 37.0, 1e-6);
}

static void test_invalid(void)
{
    uint8_t page[512] = { 0 };
    FmonSmartInfo info;

    g_assert_false(fmon_smart_parse_nvme_health(page, sizeof(page), &info));
    g_assert_false(fmon_smart_parse_ata_smart(page, sizeof(page), &info));
    g_assert_false(fmon_smart_parse_scsi_temperature(page, sizeof(page), &info));
    g_assert_false(fmon_smart_parse_nvme_health(NULL, 0, &info));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/disk_smart/nvme", test_nvme_health);
    g_test_add_func("/disk_smart/ata", test_ata_smart);
    g_test_add_func("/disk_smart/scsi", test_scsi_temperature);
    g_test_add_func("/disk_smart/nvmecontrol-text", test_nvmecontrol_text);
    g_test_add_func("/disk_smart/smartctl-text", test_smartctl_text);
    g_test_add_func("/disk_smart/invalid", test_invalid);
    return g_test_run();
}
