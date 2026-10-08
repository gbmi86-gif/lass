/* resume.c  –  binary state file for download resume */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resume.h"
#include "engine.h"
#include "segment.h"

#define RESUME_MAGIC   0x49444D52u   /* 'IDMR' */
#define RESUME_VERSION 1

static void make_state_path(const char *out_path, char *state_path, int maxlen) {
    /* حداکثر maxlen-10 بایت از out_path کپی کن تا جای پسوند باشه */
    int ext_len = (int)sizeof(IDM_STATE_FILE_EXT) - 1;
    int path_max = maxlen - ext_len - 1;
    if (path_max <= 0) { state_path[0] = '\0'; return; }
    snprintf(state_path, (size_t)maxlen, "%.*s%s",
             path_max, out_path, IDM_STATE_FILE_EXT);
}

/* ======================================================
   Save
   ====================================================== */
bool resume_save(IDM_Download *dl) {
    if (!dl) return false;

    char state_path[IDM_MAX_PATH];
    make_state_path(dl->out_path, state_path, sizeof(state_path));

    FILE *f = fopen(state_path, "wb");
    if (!f) return false;

    IDM_ResumeData hdr = {0};
    hdr.magic        = RESUME_MAGIC;
    hdr.version      = RESUME_VERSION;
    hdr.num_segments = (uint16_t)dl->num_segments;
    hdr.total_size   = dl->total_size;
    hdr.downloaded   = dl->downloaded;
    /* memcpy + explicit null چون src و dst دقیقاً همان اندازه‌ان */
    memcpy(hdr.url,      dl->url,      IDM_MAX_URL  - 1); hdr.url[IDM_MAX_URL-1]       = '\0';
    memcpy(hdr.out_path, dl->out_path, IDM_MAX_PATH - 1); hdr.out_path[IDM_MAX_PATH-1] = '\0';

    fwrite(&hdr, sizeof(hdr), 1, f);

    /* دفاعی: dl->segments می‌تونه NULL باشه (مثلاً بین teardown و
       queue شدن دوباره) درحالی‌که dl->num_segments هنوز صفر نشده.
       قبلاً این چک وجود نداشت و یه null-deref واقعی (crash) از
       اینجا اتفاق افتاده بود؛ حالا race اصلی‌ش (در engine_pause/
       engine_resume) هم بسته شده، ولی این چک به‌عنوان یه لایه‌ی
       دفاعی اضافه، مستقل از فرض‌های caller، باقی می‌مونه. */
    if (!dl->segments) { fclose(f); return true; }

    for (uint32_t i = 0; i < dl->num_segments; i++) {
        IDM_Segment *seg = dl->segments[i];
        if (!seg) continue;
        IDM_ResumeSeg rs = {0};
        rs.id      = seg->id;
        rs.start   = seg->start;
        rs.end     = seg->end;

        EnterCriticalSection(&seg->lock);
        rs.current = seg->current;
        LeaveCriticalSection(&seg->lock);

        fwrite(&rs, sizeof(rs), 1, f);
    }

    fclose(f);
    return true;
}

/* ======================================================
   Load  (dl->url and dl->out_path must already be set)
   ====================================================== */
bool resume_load(IDM_Download *dl) {
    if (!dl) return false;

    char state_path[IDM_MAX_PATH];
    make_state_path(dl->out_path, state_path, sizeof(state_path));

    FILE *f = fopen(state_path, "rb");
    if (!f) return false;

    IDM_ResumeData hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != RESUME_MAGIC          ||
        hdr.version != RESUME_VERSION) {
        fclose(f);
        return false;
    }

    /* Validate URL matches */
    if (strcmp(hdr.url, dl->url) != 0) {
        fclose(f);
        return false;
    }

    dl->total_size   = hdr.total_size;
    dl->downloaded   = hdr.downloaded;
    dl->num_segments = hdr.num_segments;

    /* Read segment offsets */
    dl->resume_segs = (IDM_ResumeSeg *)calloc(hdr.num_segments, sizeof(IDM_ResumeSeg));
    if (!dl->resume_segs) { fclose(f); return false; }

    for (uint16_t i = 0; i < hdr.num_segments; i++) {
        if (fread(&dl->resume_segs[i], sizeof(IDM_ResumeSeg), 1, f) != 1) {
            free(dl->resume_segs);
            dl->resume_segs = NULL;
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

/* ======================================================
   Remove state file
   ====================================================== */
void resume_remove(IDM_Download *dl) {
    if (!dl) return;
    char state_path[IDM_MAX_PATH];
    make_state_path(dl->out_path, state_path, sizeof(state_path));
    DeleteFileA(state_path);
}
