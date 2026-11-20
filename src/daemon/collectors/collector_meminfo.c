/*
 * collector_meminfo.c - 内存条信息（dmidecode，需要 root）
 *
 * 采集内容：插槽总数、每条内存的容量/类型/标称与实际速率/厂商。
 * dmidecode 需要读取 /dev/mem，因此只能在守护进程（root）里运行；
 * 每 60 秒刷新一次，系统运行期间内存不会热插拔，开销可忽略。
 */

#include "fmon_daemon.h"
#include "fmon_sysctl.h"

#include "fmon_child.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 解析 "16 GB" / "512 MB" 为 MB */
static uint32_t parse_size_mb(const char *v)
{
    if (v == NULL || v[0] == '\0' || strstr(v, "No Module") != NULL)
        return 0;

    unsigned long n = strtoul(v, NULL, 10);

    if (strstr(v, "GB") != NULL || strstr(v, "gb") != NULL ||
        strstr(v, "GiB") != NULL || strstr(v, "TiB") != NULL)
        n *= 1024;

    if (n > 1024UL * 1024UL)
        n = 1024UL * 1024UL;   /* 上限 1 TB，防止异常数据 */

    /* 某些 dmidecode 版本只给出数字（单位被截断），现代内存条最小也是 4 GB；
     * 小于 128 的数值按 GB 处理，避免把 16 GB 显示成 16 MB。 */
    if (n > 0 && n < 128)
        n *= 1024;

    return (uint32_t)n;
}

static uint32_t parse_speed(const char *v)
{
    if (v == NULL)
        return 0;

    unsigned long n = strtoul(v, NULL, 10);

    return (n > 100000) ? 0 : (uint32_t)n;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;

    char *end = s + strlen(s);

    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r'))
        *--end = '\0';

    return s;
}

static void parse_dmidecode(const char *out, FmonMemModule *mods, int *count,
                            uint32_t *slots)
{
    char *copy = strdup(out);
    char *save = NULL;

    FmonMemModule cur;
    int           in_dev = 0;

    memset(&cur, 0, sizeof(cur));
    *count = 0;
    *slots = 0;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        if (strncmp(line, "Memory Device", 13) == 0) {
            /* 新的一条：先把上一条收尾 */
            if (in_dev) {
                (*slots)++;
                if (cur.size_mb > 0 && *count < FMON_MAX_MEM_MODULES)
                    mods[(*count)++] = cur;
            }

            memset(&cur, 0, sizeof(cur));
            in_dev = 1;
            continue;
        }

        if (!in_dev)
            continue;

        char *colon = strchr(line, ':');

        if (colon == NULL)
            continue;

        *colon = '\0';

        char *key = trim(line);
        char *val = trim(colon + 1);

        if (strcmp(key, "Size") == 0) {
            cur.size_mb = parse_size_mb(val);
            cur.used    = (cur.size_mb > 0) ? 1u : 0u;
        } else if (strcmp(key, "Type") == 0 && strcmp(val, "Unknown") != 0) {
            strlcpy(cur.type, val, sizeof(cur.type));
        } else if (strcmp(key, "Speed") == 0) {
            cur.speed_mts = parse_speed(val);
        } else if (strcmp(key, "Configured Memory Speed") == 0) {
            cur.configured_mts = parse_speed(val);
        } else if (strcmp(key, "Locator") == 0) {
            strlcpy(cur.locator, val, sizeof(cur.locator));
        } else if (strcmp(key, "Bank Locator") == 0) {
            strlcpy(cur.bank, val, sizeof(cur.bank));
        } else if (strcmp(key, "Manufacturer") == 0) {
            if (strcmp(val, "Unknown") != 0 &&
                strcmp(val, "Not Specified") != 0)
                strlcpy(cur.manufacturer, val, sizeof(cur.manufacturer));
        }
    }

    if (in_dev) {
        (*slots)++;
        if (cur.size_mb > 0 && *count < FMON_MAX_MEM_MODULES)
            mods[(*count)++] = cur;
    }

    free(copy);
}

void *thread_meminfo(void *arg)
{
    (void)arg;

    FmonMemModule mods[FMON_MAX_MEM_MODULES];

    while (g_running) {
        memset(mods, 0, sizeof(mods));

        int      count = 0;
        uint32_t slots = 0;

        const char *const argv[] = { "dmidecode", "-t", "17", NULL };
        FmonChildResult res;
        bool ok = false;

        if (fmon_child_run(argv, 256 * 1024, 5000, &res) == 0 &&
            fmon_child_ok(&res) && res.out != NULL) {
            parse_dmidecode(res.out, mods, &count, &slots);
            ok = true;
        }

        fmon_child_result_free(&res);

        fmon_shm_wr_begin();
        if (ok) {
            g_shm->probe_valid |= FMON_PROBE_MEM_MODULES;
            g_shm->mem_slot_total   = slots;
            g_shm->mem_module_count = (uint32_t)count;
            memcpy(g_shm->mem_modules, mods, sizeof(mods));
        } else {
            g_shm->probe_valid &= ~FMON_PROBE_MEM_MODULES;
            g_shm->mem_slot_total   = 0;
            g_shm->mem_module_count = 0;
            memset(g_shm->mem_modules, 0, sizeof(g_shm->mem_modules));
        }
        fmon_shm_wr_end();

        if (!fmon_sleep_ms(60000) && !g_running)
            break;
    }

    return NULL;
}
