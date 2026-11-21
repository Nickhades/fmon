/*
 * history_parse.h - history TSV 行解析与分桶
 */

#ifndef FMON_HISTORY_PARSE_H
#define FMON_HISTORY_PARSE_H

#include <glib.h>

#include "history/history_provider.h"

typedef struct {
    long long ts;
    double    v[15];
    unsigned  samples;
    bool      old_format;
} HistoryRow;

extern double   s_values[FMON_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
extern double   s_min[FMON_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
extern double   s_max[FMON_HIST_COUNT][FMON_HISTORY_MAX_POINTS];
extern uint64_t s_time_us[FMON_HISTORY_MAX_POINTS];
extern FmonHistorySeries s_series[FMON_HIST_COUNT];
extern uint32_t s_count;
extern uint32_t s_interval_s;
extern bool     s_loaded;

GArray *history_read_rows(const char *path, gint64 cutoff, bool raw,
                          bool *old_format);
void    history_bucket_rows(const GArray *rows, uint32_t base_interval_s);

#endif
