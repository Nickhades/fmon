/*
 * fmon_smart.c - 直接解析内核返回的 SMART/温度二进制页
 *
 * 不调用 nvmecontrol、smartctl 或任何外部程序；所有偏移来自
 * NVMe Base Spec / ATA SMART / SCSI LOG SENSE 标准结构。
 */

#include "fmon_smart.h"

#include <stdlib.h>
#include <string.h>

static void smart_init(FmonSmartInfo *out)
{
    if (out == NULL)
        return;

    out->valid     = false;
    out->temp_c    = -1.0;
    out->spare_pct = -1;
    out->wear_pct  = -1;
}

bool fmon_smart_parse_nvme_health(const uint8_t *buf, size_t len,
                                  FmonSmartInfo *out)
{
    smart_init(out);

    if (buf == NULL || out == NULL || len < 6)
        return false;

    /* struct nvme_health_information_page（packed）：
     *   [0] critical_warning
     *   [1..2] temperature（Kelvin，小端）
     *   [3] available_spare
     *   [4] available_spare_threshold
     *   [5] percentage_used
     */
    uint16_t kelvin = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);

    if (kelvin == 0 || kelvin == 0xffff)
        return false;

    double c = (double)kelvin - 273.15;

    if (c < -50.0 || c > 200.0)
        return false;

    out->temp_c    = c;
    out->spare_pct = (int)buf[3];
    out->wear_pct  = (int)buf[5];
    out->valid     = true;
    return true;
}

static bool ata_attr_temp(const uint8_t *a, int *temp)
{
    /* ATA SMART attribute entry（12 字节）：
     *   [0] ID, [1..2] flags, [3] value, [4] worst,
     *   [5..10] raw[6], [11] reserved
     * 194 = Temperature_Celsius, 190 = Airflow_Temperature_Cel。 */
    uint8_t id = a[0];

    if (id != 194 && id != 190)
        return false;

    int t = a[5];   /* 常见 ATA 盘的 raw[0] 就是摄氏度 */

    if (t <= 0 || t >= 120)
        return false;

    *temp = t;
    return true;
}

bool fmon_smart_parse_ata_smart(const uint8_t *buf, size_t len,
                                FmonSmartInfo *out)
{
    smart_init(out);

    if (buf == NULL || out == NULL || len < 362)
        return false;

    for (int i = 0; i < 30; i++) {
        int t = 0;

        if (ata_attr_temp(buf + 2 + i * 12, &t)) {
            out->temp_c = (double)t;
            out->valid  = true;
            return true;
        }
    }

    return false;
}

static const char *line_start(const char *line, const char *end)
{
    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    return line;
}

static bool parse_temp_value(const char *text, double *out)
{
    char  *ep = NULL;
    double v  = strtod(text, &ep);

    if (ep == text || v < -50.0 || v > 200.0)
        return false;

    *out = v;
    return true;
}

bool fmon_smart_parse_nvmecontrol(const char *text, FmonSmartInfo *out)
{
    smart_init(out);

    if (text == NULL || out == NULL)
        return false;

    const char *line = text;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *p   = line_start(line, end != NULL ? end : line + strlen(line));

        if (strncmp(p, "Temperature:", 12) == 0) {
            const char *c = strstr(p, " K, ");

            if (c != NULL) {
                double v;
                if (parse_temp_value(c + 4, &v)) {
                    out->temp_c = v;
                    out->valid  = true;
                }
            }
        } else if (strncmp(p, "Available spare:", 16) == 0) {
            const char *colon = strchr(p, ':');
            if (colon != NULL) {
                int n = (int)strtol(colon + 1, NULL, 10);
                if (n >= 0 && n <= 100) {
                    out->spare_pct = n;
                    out->valid     = true;
                }
            }
        } else if (strncmp(p, "Percentage used:", 16) == 0) {
            const char *colon = strchr(p, ':');
            if (colon != NULL) {
                int n = (int)strtol(colon + 1, NULL, 10);
                if (n >= 0 && n <= 100) {
                    out->wear_pct = n;
                    out->valid    = true;
                }
            }
        }

        if (end == NULL)
            break;
        line = end + 1;
    }

    return out->valid;
}

bool fmon_smart_parse_smartctl(const char *text, FmonSmartInfo *out)
{
    smart_init(out);

    if (text == NULL || out == NULL)
        return false;

    const char *line = text;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *p   = line_start(line, end != NULL ? end : line + strlen(line));

        if (strstr(p, "Temperature") != NULL ||
            strstr(p, "temperature") != NULL) {
            const char *colon = strchr(p, ':');

            if (colon != NULL && strstr(p, " K, ") == NULL) {
                double t;

                if (parse_temp_value(colon + 1, &t)) {
                    out->temp_c = t;
                    out->valid  = true;
                }
            } else if (colon == NULL &&
                       (strncmp(p, "194", 3) == 0 ||
                        strncmp(p, "190", 3) == 0)) {
                /* ATA attribute 行：... Always - 35 (Min/Max 20/45) */
                const char *paren = strchr(p, '(');
                const char *q     = (paren != NULL) ? paren : p + strlen(p);

                while (q > p && (*(q - 1) < '0' || *(q - 1) > '9') &&
                       *(q - 1) != '-')
                    q--;

                const char *start = q;

                while (start > p &&
                       ((*(start - 1) >= '0' && *(start - 1) <= '9') ||
                        *(start - 1) == '-'))
                    start--;

                double t;

                if (parse_temp_value(start, &t)) {
                    out->temp_c = t;
                    out->valid  = true;
                }
            }
        } else if (strncmp(p, "Available Spare:", 16) == 0 ||
                   strncmp(p, "Available spare:", 16) == 0) {
            const char *colon = strchr(p, ':');

            if (colon != NULL) {
                int n = (int)strtol(colon + 1, NULL, 10);
                if (n >= 0 && n <= 100) {
                    out->spare_pct = n;
                    out->valid     = true;
                }
            }
        } else if (strncmp(p, "Percentage Used:", 16) == 0 ||
                   strncmp(p, "Percentage used:", 16) == 0) {
            const char *colon = strchr(p, ':');

            if (colon != NULL) {
                int n = (int)strtol(colon + 1, NULL, 10);
                if (n >= 0 && n <= 100) {
                    out->wear_pct = n;
                    out->valid    = true;
                }
            }
        }

        if (end == NULL)
            break;
        line = end + 1;
    }

    return out->valid;
}

bool fmon_smart_parse_scsi_temperature(const uint8_t *buf, size_t len,
                                       FmonSmartInfo *out)
{
    smart_init(out);

    if (buf == NULL || out == NULL || len < 8)
        return false;

    /* LOG SENSE 返回：page code [0], page length [2..3]（大端），
     * 参数从 offset 4 开始；每个参数头 4 字节（code/control/length）。 */
    if ((buf[0] & 0x3f) != 0x0d)
        return false;

    size_t page_len = ((size_t)buf[2] << 8) | buf[3];
    size_t end      = 4 + page_len;

    if (end > len)
        end = len;

    size_t off = 4;

    while (off + 4 <= end) {
        uint16_t code = (uint16_t)buf[off] << 8 | buf[off + 1];
        size_t   plen = buf[off + 3];

        if (code == 0x0000 && plen >= 2 && off + 4 + plen <= end) {
            int t = buf[off + 5];   /* header + reserved + temperature */

            if (t > 0 && t < 120) {
                out->temp_c = (double)t;
                out->valid  = true;
                return true;
            }
        }

        off += 4 + plen;
    }

    return false;
}
