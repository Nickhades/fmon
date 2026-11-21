/* collector_gpu_nvidia.h - NVIDIA 采集 */
#ifndef FMON_COLLECTOR_GPU_NVIDIA_H
#define FMON_COLLECTOR_GPU_NVIDIA_H

#include "fmon_shm.h"

void gpu_sample_nvml(FmonShmData *s, int n);

#endif
