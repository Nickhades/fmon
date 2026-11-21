/* history_sidecars.h */
#ifndef FMON_HISTORY_SIDECARS_H
#define FMON_HISTORY_SIDECARS_H

#include "fmon_shm.h"
#include <time.h>

void history_gpu_sample(time_t now, const FmonShmData *s);
void history_zfs_sample(time_t now, const FmonShmData *s);

#endif
