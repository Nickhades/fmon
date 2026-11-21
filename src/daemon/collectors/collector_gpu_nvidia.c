/*
 * collector_gpu_nvidia.c - NVIDIA NVML/nvidia-smi 采集
 */

#include "fmon_daemon.h"
#include "fmon_sysctl.h"
#include "fmon_child.h"
#include "collector_gpu_nvidia.h"
#include "collector_gpu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double csv_number(const char *s)
{
    if (s == NULL || s[0] == '\0' || s[0] == '[')
        return -1.0;

    return strtod(s, NULL);
}

/* nvidia-smi -q 的补充明细：BAR1 / ECC / P-State */
static void parse_nvml_q(const char *out, FmonGpuInfo **gpus, int nv_count)
{
    char *copy = strdup(out);
    char *save = NULL;
    int   ord  = -1;
    int   section = 0;   /* 1=BAR1 2=ECC */

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        while (*line == ' ' || *line == '\t')
            line++;

        if (strncmp(line, "GPU 0000", 8) == 0) {
            ord++;
            section = 0;
            continue;
        }

        if (ord < 0 || ord >= nv_count)
            continue;

        FmonGpuInfo *g = gpus[ord];

        if (strncmp(line, "BAR1 Memory Usage", 17) == 0) {
            section = 1;
            continue;
        }
        if (strncmp(line, "ECC Mode", 8) == 0) {
            section = 2;
            continue;
        }
        if (strncmp(line, "Performance State", 17) == 0) {
            char st[16] = {0};

            if (sscanf(line, "Performance State : P%15s", st) == 1)
                g->pstate = (uint32_t)strtoul(st, NULL, 10);
            section = 0;
            continue;
        }
        if (strncmp(line, "Single Bit ECC", 14) == 0) {
            char v[32] = {0};

            if (sscanf(line, "Single Bit ECC : %31s", v) == 1 && v[0] != 'N')
                g->ecc_sbe = strtoull(v, NULL, 10);
            continue;
        }
        if (strncmp(line, "Double Bit ECC", 14) == 0) {
            char v[32] = {0};

            if (sscanf(line, "Double Bit ECC : %31s", v) == 1 && v[0] != 'N')
                g->ecc_dbe = strtoull(v, NULL, 10);
            continue;
        }

        if (section == 1) {
            char *colon = strchr(line, ':');

            if (colon != NULL) {
                double mb = strtod(colon + 1, NULL);

                if (strncmp(line, "Total", 5) == 0)
                    g->bar1_total_mb = mb;
                else if (strncmp(line, "Used", 4) == 0)
                    g->bar1_used_mb = mb;
            }
        } else if (section == 2) {
            if (strncmp(line, "Current", 7) == 0) {
                if (strstr(line, "Enabled") != NULL)
                    g->ecc_mode = 2;
                else if (strstr(line, "Disabled") != NULL)
                    g->ecc_mode = 1;
            }
        }
    }

    free(copy);
}

/* nvidia-smi dmon -s t：实时 PCIe 吞吐（MB/s） */
static void parse_nvml_pcie(const char *out, FmonGpuInfo **gpus, int nv_count)
{
    char *copy = strdup(out);
    char *save = NULL;

    for (char *line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        if (line[0] == '#')
            continue;

        int    idx = -1;
        double rx = 0.0, tx = 0.0;

        if (sscanf(line, " %d %lf %lf", &idx, &rx, &tx) == 3 && idx >= 0 &&
            idx < nv_count) {
            gpus[idx]->pcie_rx_mbps = rx;
            gpus[idx]->pcie_tx_mbps = tx;
        }
    }

    free(copy);
}

void gpu_sample_nvml(FmonShmData *s, int n)
{
    /* 必须用绝对路径：rc.d 环境下进程的 PATH 不含 /usr/local/bin，
     * 直接 execvp("nvidia-smi") 会得到 127（找不到文件）。
     * 支持 FMON_NVIDIA_SMI 覆盖（自编译驱动可放在任意位置）。 */
    static char nvsmi[1024];

    if (nvsmi[0] == '\0') {
        const char *env = getenv("FMON_NVIDIA_SMI");

        if (env != NULL && env[0] != '\0' && access(env, X_OK) == 0)
            strlcpy(nvsmi, env, sizeof(nvsmi));
        else if (!gpu_resolve_binary("nvidia-smi", nvsmi, sizeof(nvsmi)))
            return;
    }

    const char *argv[] = {
        nvsmi,
        "--query-gpu=name,driver_version,vbios_version,serial,uuid,"
        "memory.total,memory.used,memory.free,"
        "utilization.gpu,utilization.memory,utilization.encoder,"
        "utilization.decoder,utilization.jpeg,utilization.ofa,"
        "clocks.current.graphics,clocks.current.sm,clocks.current.memory,"
        "clocks.current.video,temperature.gpu,fan.speed,power.draw,"
        "power.limit,pcie.link.gen.current,pcie.link.width.current",
        "--format=csv,noheader,nounits",
        NULL
    };

    FmonChildResult res;
    if (fmon_child_run(argv, 128 * 1024, 3000, &res) != 0 ||
        !fmon_child_ok(&res) || res.out == NULL) {
        fmon_child_result_free(&res);
        return;   /* 保持 load_available = 0 */
    }

    bool filled[FMON_MAX_GPUS] = { false };
    int  line_no = 0;
    char *save   = NULL;

    for (char *line = strtok_r(res.out, "\n", &save);
         line != NULL && line_no < n;
         line = strtok_r(NULL, "\n", &save)) {

        char *pos = line;
        char  f[24][64];
        int   got = 0;

        while (got < 24) {
            if (gpu_csv_next(&pos, f[got], sizeof(f[got])) == NULL)
                break;
            got++;
        }

        if (got < 5)
            continue;

        char *name = f[0];

        int idx = -1;

        for (int j = 0; j < n; j++) {
            if (!filled[j] && g_descs[j].sensor == FMON_GPU_SENSOR_NVML &&
                strcmp(g_descs[j].name, name) == 0) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            for (int j = 0; j < n; j++) {
                if (!filled[j] && g_descs[j].sensor == FMON_GPU_SENSOR_NVML) {
                    idx = j;
                    break;
                }
            }
        }
        if (idx < 0)
            continue;

        FmonGpuInfo *g = &s->gpus[idx];

        g->load_available   = 1;
        g->usage_pct        = csv_number(f[8]);
        g->mem_used_mb      = csv_number(f[6]);
        g->mem_total_mb     = csv_number(f[5]);
        g->temp_c           = csv_number(f[18]);
        g->clock_mhz        = (uint32_t)((csv_number(f[14]) > 0.0)
                                             ? csv_number(f[14])
                                             : 0.0);

        if (f[1][0] != '[')
            strlcpy(g->driver_version, f[1], sizeof(g->driver_version));
        if (f[2][0] != '[')
            strlcpy(g->vbios_version, f[2], sizeof(g->vbios_version));
        if (f[3][0] != '[')
            strlcpy(g->serial, f[3], sizeof(g->serial));
        if (f[4][0] != '[')
            strlcpy(g->uuid, f[4], sizeof(g->uuid));

        g->util_mem_pct    = csv_number(f[9]);
        g->util_enc_pct    = csv_number(f[10]);
        g->util_dec_pct    = csv_number(f[11]);
        g->util_jpeg_pct   = csv_number(f[12]);
        g->util_ofa_pct    = csv_number(f[13]);
        g->clock_sm_mhz    = csv_number(f[15]);
        g->clock_mem_mhz   = csv_number(f[16]);
        g->clock_video_mhz = csv_number(f[17]);
        g->fan_pct         = csv_number(f[19]);
        g->power_w         = csv_number(f[20]);
        g->power_limit_w   = csv_number(f[21]);
        g->pcie_gen        = (uint32_t)((csv_number(f[22]) > 0.0)
                                            ? csv_number(f[22])
                                            : 0.0);
        g->pcie_width      = (uint32_t)((csv_number(f[23]) > 0.0)
                                            ? csv_number(f[23])
                                            : 0.0);

        filled[idx] = true;
        line_no++;
    }

    fmon_child_result_free(&res);

    /* 每 10 秒补充 BAR1 / ECC / PCIe 吞吐 */
    static int64_t next_detail_ms = 0;
    int64_t        now = fmon_now_ms();

    if (now < next_detail_ms)
        return;

    next_detail_ms = now + 10000;

    FmonGpuInfo *nv[FMON_MAX_GPUS];
    int          nv_count = 0;

    for (int j = 0; j < n && nv_count < FMON_MAX_GPUS; j++) {
        if (g_descs[j].sensor == FMON_GPU_SENSOR_NVML)
            nv[nv_count++] = &s->gpus[j];
    }

    if (nv_count == 0)
        return;

    const char *qargv[] = { nvsmi, "-q", NULL };
    FmonChildResult qres;

    if (fmon_child_run(qargv, 256 * 1024, 3000, &qres) == 0 &&
        fmon_child_ok(&qres) && qres.out != NULL)
        parse_nvml_q(qres.out, nv, nv_count);

    fmon_child_result_free(&qres);

    const char *dargv[] = { nvsmi, "dmon", "-s", "t", "-c", "1", NULL };
    FmonChildResult dres;

    if (fmon_child_run(dargv, 64 * 1024, 3000, &dres) == 0 &&
        fmon_child_ok(&dres) && dres.out != NULL)
        parse_nvml_pcie(dres.out, nv, nv_count);

    fmon_child_result_free(&dres);
}
