/* storage.h  –  thread-safe random-access file writer */
#ifndef IDM_STORAGE_H
#define IDM_STORAGE_H

#include "types.h"

typedef struct IDM_FileWriter IDM_FileWriter;

IDM_FileWriter *fw_open (const char *path, uint64_t expected_size, bool prealloc);
int             fw_write(IDM_FileWriter *fw, uint64_t offset, const void *data, size_t len);
int             fw_read (IDM_FileWriter *fw, uint64_t offset, void *buf,        size_t len);
void            fw_sync (IDM_FileWriter *fw);
uint64_t        fw_size (IDM_FileWriter *fw);
void            fw_close(IDM_FileWriter *fw);

#endif /* IDM_STORAGE_H */
