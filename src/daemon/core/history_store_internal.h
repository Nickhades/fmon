/* history_store_internal.h */
#ifndef FMON_HISTORY_STORE_INTERNAL_H
#define FMON_HISTORY_STORE_INTERNAL_H

void history_store_mkdir_p(const char *path);
void history_store_rotate_if_needed(const char *path);

#endif
