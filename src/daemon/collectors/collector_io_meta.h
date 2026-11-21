/*
 * collector_io_meta.h - 磁盘型号/容量/CAM 元数据缓存
 */

#ifndef FMON_COLLECTOR_IO_META_H
#define FMON_COLLECTOR_IO_META_H

#include "fmon_shm.h"

void disk_io_meta_invalidate(const char *name);
void disk_io_fill_meta(FmonDiskInfo *di, int64_t now);

#endif
