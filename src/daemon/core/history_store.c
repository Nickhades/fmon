/*
 * history_store.c - 指标历史持久化实现
 */

#include "history_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HISTORY_MAX_BYTES  (20UL * 1024UL * 1024UL)

static char g_path[512];
static bool g_path_ready = false;

/* 逐级创建目录（最简单的 mkdir -p） */
static void mkdir_p(const char *path)
{
    char   buf[512];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(buf))
        return;

    snprintf(buf, sizeof(buf), "%s", path);

    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        (void)mkdir(buf, 0755);
        *p = '/';
    }
    (void)mkdir(buf, 0755);
}

const char *fmon_history_store_path(void)
{
    if (g_path_ready)
        return g_path;

    if (geteuid() == 0) {
        snprintf(g_path, sizeof(g_path), "/var/db/fmon/history.tsv");
    } else {
        const char *home = getenv("HOME");

        if (home == NULL || home[0] == '\0')
            home = "/tmp";
        snprintf(g_path, sizeof(g_path),
                 "%s/.local/state/fmon/history.tsv", home);
    }

    g_path_ready = true;
    return g_path;
}

/* 文件超过 20 MiB 时轮转一次 */
static void rotate_if_needed(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return;
    if ((unsigned long)st.st_size < HISTORY_MAX_BYTES)
        return;

    char old[512];

    snprintf(old, sizeof(old), "%s.1", path);
    (void)unlink(old);
    (void)rename(path, old);
}

void fmon_history_store_sample(const FmonShmData *s)
{
    if (s == NULL)
        return;

    const char *path = fmon_history_store_path();
    char        dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash != NULL) {
        *slash = '\0';
        mkdir_p(dir);
    }

    rotate_if_needed(path);

    FILE *fp = fopen(path, "a");

    if (fp == NULL)
        return;

    (void)fprintf(fp, "%lld\t%.2f\t%.2f\t%.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
                  (long long)time(NULL), s->cpu_usage_pct, s->mem_used_pct,
                  s->swap_used_pct, s->net_rx_kbps, s->net_tx_kbps,
                  s->disk_read_kbps, s->disk_write_kbps, s->cpu_temp_c);

    (void)fclose(fp);
}
