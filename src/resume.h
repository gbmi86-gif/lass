/* resume.h  –  save/load download state for resume support */
#ifndef IDM_RESUME_H
#define IDM_RESUME_H

#include "types.h"

typedef struct IDM_ResumeData {
    uint32_t magic;          /* 0x49444D52 = 'IDMR' */
    uint16_t version;        /* 1 */
    uint16_t num_segments;
    char     url[IDM_MAX_URL];
    char     out_path[IDM_MAX_PATH];
    uint64_t total_size;
    uint64_t downloaded;
    /* followed by num_segments * ResumeSegment structs */
} IDM_ResumeData;

typedef struct {
    uint32_t id;
    uint64_t start;
    uint64_t end;
    uint64_t current;     /* resume from here */
} IDM_ResumeSeg;

/* Save state to <out_path>.idmstate */
bool resume_save(IDM_Download *dl);
/* Load state from <out_path>.idmstate into dl (already constructed) */
bool resume_load(IDM_Download *dl);
/* Delete state file */
void resume_remove(IDM_Download *dl);

#endif /* IDM_RESUME_H */
