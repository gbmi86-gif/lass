/* media_store.c — کانتینر، نام فایل و اثر انگشت مشترک؛ رجوع کن به media_store.h */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_store.h"

#define MEDIA_READ_BLOCK     (256u * 1024u)
#define MEDIA_SUFFIX_ROOM    32u

static void copy_str(char *dst, size_t dst_sz, const char *src) {
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

TelsId media_entry_find(TelsContainer *c, const char *name) {
    TelsId id = 0;
    return tels_lookup(c, TELS_ROOT, name, &id) ? id : 0;
}

BOOL media_entry_exists(TelsContainer *c, const char *name) {
    return media_entry_find(c, name) != 0;
}

BOOL media_entry_size(TelsContainer *c, const char *name, uint64_t *sz) {
    TelsId id = media_entry_find(c, name);
    return id != 0 && tels_size(c, id, sz);
}

BOOL media_entry_drop(TelsContainer *c, const char *name) {
    TelsId id = media_entry_find(c, name);
    return id ? tels_delete(c, id) : TRUE;
}

BOOL media_entry_put(TelsContainer *c, const char *name, const uint8_t *data,
                     uint64_t len, uint32_t chunk) {
    TelsId id = 0;
    uint64_t off = 0;
    if (chunk == 0) return FALSE;
    if (!media_entry_drop(c, name)) return FALSE;
    if (!tels_create(c, TELS_ROOT, name, TELS_TYPE_FILE, &id)) return FALSE;
    while (off < len) {
        uint64_t left = len - off;
        uint32_t n = left > chunk ? chunk : (uint32_t)left;
        if (!tels_write(c, id, off, data + off, n)) return FALSE;
        off += n;
    }
    return TRUE;
}

BOOL media_entry_marker(TelsContainer *c, const char *name) {
    TelsId id = 0;
    if (!media_entry_drop(c, name)) return FALSE;
    return tels_create(c, TELS_ROOT, name, TELS_TYPE_FILE, &id);
}

BOOL media_entry_read(TelsContainer *c, const char *name, MediaBuf *out, uint64_t max_len) {
    TelsId id = media_entry_find(c, name);
    uint64_t size = 0, off = 0;
    out->len = 0;
    if (!id || !tels_size(c, id, &size)) return FALSE;
    if (size > max_len || !media_buf_reserve(out, size, max_len)) return FALSE;
    while (off < size) {
        uint64_t left = size - off;
        uint32_t want = left > MEDIA_READ_BLOCK ? MEDIA_READ_BLOCK : (uint32_t)left;
        if (tels_read(c, id, off, out->data + off, want) != (int32_t)want) return FALSE;
        off += want;
    }
    out->len = size;
    return TRUE;
}

BOOL media_entry_copy_to_handle(TelsContainer *c, const char *name, HANDLE h,
                                uint8_t *buf, uint32_t chunk) {
    TelsId id = media_entry_find(c, name);
    uint64_t size = 0, off = 0;
    if (!id || !tels_size(c, id, &size) || chunk == 0) return FALSE;
    while (off < size) {
        uint64_t left = size - off;
        uint32_t want = left > chunk ? chunk : (uint32_t)left;
        DWORD w = 0;
        if (tels_read(c, id, off, buf, want) != (int32_t)want) return FALSE;
        if (!WriteFile(h, buf, want, &w, NULL) || w != want) return FALSE;
        off += want;
    }
    return TRUE;
}

BOOL media_container_clear(TelsContainer *c) {
    uint32_t n = tels_list(c, TELS_ROOT, NULL, 0);
    TelsId *ids;
    BOOL ok = TRUE;
    uint32_t i;
    if (n == 0) return TRUE;
    ids = (TelsId *)malloc(sizeof(TelsId) * n);
    if (!ids) return FALSE;
    n = tels_list(c, TELS_ROOT, ids, n);
    for (i = 0; i < n; i++) ok = tels_delete(c, ids[i]) && ok;
    free(ids);
    return ok;
}

void media_container_finish(TelsContainer *c) {
    if (c) {
        tels_commit(c);
        tels_close(c);
    }
}

TelsContainer *media_container_open(const char *path, uint32_t tries, const MediaHttp *h) {
    uint32_t t;
    for (t = 0; t < tries; t++) {
        TelsContainer *c = tels_open(path, TRUE);
        if (c) return c;
        if (!media_sleep_checked(h, 1000u)) return NULL;
    }
    return NULL;
}

BOOL media_path_exists(const char *p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

BOOL media_build_candidate(char *out, size_t sz, const char *stem, int n, const char *ext) {
    char num[16];
    size_t sl, nl, el;
    num[0] = '\0';
    if (n > 0) snprintf(num, sizeof(num), " (%d)", n);
    sl = strlen(stem);
    nl = strlen(num);
    el = strlen(ext);
    if (sl + nl + el + 1u > sz) return FALSE;
    memcpy(out, stem, sl);
    memcpy(out + sl, num, nl);
    memcpy(out + sl + nl, ext, el + 1u);
    return TRUE;
}

void media_choose_final_path(const char *out_path, const char *ext,
                             const char *const *strip, size_t n_strip,
                             char *out, size_t sz) {
    char stem[IDM_MAX_PATH];
    char *dot, *sep, *sep2;
    size_t room, i;
    int n;
    copy_str(stem, sizeof(stem), out_path);
    dot = strrchr(stem, '.');
    sep = strrchr(stem, '\\');
    sep2 = strrchr(stem, '/');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
    if (dot && (!sep || dot > sep)) {
        for (i = 0; i < n_strip; i++) {
            if (_stricmp(dot, strip[i]) == 0) {
                *dot = '\0';
                break;
            }
        }
    }
    room = sz > MEDIA_SUFFIX_ROOM ? sz - MEDIA_SUFFIX_ROOM : 0u;
    if (strlen(stem) > room) stem[room] = '\0';
    for (n = 0; n < 1000; n++) {
        if (media_build_candidate(out, sz, stem, n, ext) && !media_path_exists(out)) return;
    }
    media_build_candidate(out, sz, stem, 0, ext);
}

uint64_t media_fnv_mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void media_strip_query(const char *url, char *out, size_t sz) {
    size_t n = 0;
    if (sz == 0) return;
    while (url[n] && url[n] != '?' && n + 1 < sz) {
        out[n] = url[n];
        n++;
    }
    out[n] = '\0';
}

int media_meta_format(char *buf, size_t sz, const char *head, uint64_t fp,
                      BOOL has_segs, uint32_t segs, const char *final_name) {
    int n;
    if (has_segs)
        n = snprintf(buf, sz, "%sfp=%016llx\nsegments=%u\nfinal=%s\n", head,
                     (unsigned long long)fp, (unsigned)segs, final_name);
    else
        n = snprintf(buf, sz, "%sfp=%016llx\nfinal=%s\n", head,
                     (unsigned long long)fp, final_name);
    if (n < 0 || (size_t)n >= sz) return -1;
    return n;
}

BOOL media_meta_parse(const char *text, const char *head, BOOL has_segs,
                      uint64_t *fp, uint32_t *segs, char *final_name, size_t fn_sz) {
    const char *f, *s = NULL, *n, *nb, *ne;
    size_t nl;
    if (strncmp(text, head, strlen(head)) != 0) return FALSE;
    f = strstr(text, "\nfp=");
    n = strstr(text, "\nfinal=");
    if (has_segs) s = strstr(text, "\nsegments=");
    if (!f || !n || (has_segs && !s)) return FALSE;
    *fp = (uint64_t)strtoull(f + 4, NULL, 16);
    if (has_segs) *segs = (uint32_t)strtoul(s + 10, NULL, 10);
    nb = n + 7;
    ne = strchr(nb, '\n');
    nl = ne ? (size_t)(ne - nb) : strlen(nb);
    if (nl == 0 || nl >= fn_sz) return FALSE;
    memcpy(final_name, nb, nl);
    final_name[nl] = '\0';
    return TRUE;
}

BOOL media_meta_load(TelsContainer *c, const char *name, const char *head, BOOL has_segs,
                     uint64_t *fp, uint32_t *segs, char *final_name, size_t fn_sz) {
    TelsId id = media_entry_find(c, name);
    uint64_t sz = 0;
    char text[1025];
    if (!id || !tels_size(c, id, &sz) || sz == 0 || sz > 1024u) return FALSE;
    if (tels_read(c, id, 0, text, (uint32_t)sz) != (int32_t)sz) return FALSE;
    text[sz] = '\0';
    return media_meta_parse(text, head, has_segs, fp, segs, final_name, fn_sz);
}
