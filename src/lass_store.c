/* lass_store.c — فروشگاه داده‌ی برنامه روی container .tels (رجوع کن به lass_store.h)

   هر مقدار (پوشه، نام) به‌صورت entry هایی به نام "name#<gen>" در container است.
   نوشتن: نسل جدید ساخته و نوشته می‌شود، بعد نسل‌های قدیمی حذف می‌شوند و یک commit
   انجام می‌شود. ترتیب رکوردها در log تضمین می‌کند که حذف نسل قدیمی فقط بعد از کامل
   شدن نسل جدید بازیابی شود؛ پس در هر کرشی یا داده‌ی قبلی یا داده‌ی جدید موجود است. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lass_store.h"
#include "tels_container.h"
#include "lass_log.h"
#include "lass_paths.h"

#define STORE_CHUNK      (4u * 1024u * 1024u)
#define STORE_GEN_DIGITS 20u

static TelsContainer    *g_store = NULL;
static char              g_store_path[MAX_PATH + 16];
static CRITICAL_SECTION  g_store_lock;
static volatile LONG     g_store_lock_state = 0;   /* 0 خام، 1 در حال ساخت، 2 آماده */

static void store_lock_init(void) {
    if (g_store_lock_state == 2) return;
    if (InterlockedCompareExchange(&g_store_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_store_lock);
        InterlockedExchange(&g_store_lock_state, 2);
    } else {
        while (g_store_lock_state != 2) Sleep(0);
    }
}

static BOOL path_join(char *out, size_t sz, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 1 > sz) return FALSE;
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return TRUE;
}

/* "name#<gen>" → gen؛ در غیر این صورت FALSE */
static BOOL parse_gen(const char *entry, const char *name, uint64_t *gen) {
    size_t nl = strlen(name);
    if (strncmp(entry, name, nl) != 0 || entry[nl] != '#') return FALSE;
    const char *d = entry + nl + 1;
    size_t dl = strlen(d);
    if (dl == 0 || dl > STORE_GEN_DIGITS) return FALSE;
    uint64_t v = 0;
    for (size_t i = 0; i < dl; i++) {
        if (d[i] < '0' || d[i] > '9') return FALSE;
        v = v * 10u + (uint64_t)(d[i] - '0');
    }
    *gen = v;
    return TRUE;
}

/* بزرگ‌ترین نسل فایل‌های name داخل پوشه‌ی dir */
static BOOL find_newest(TelsContainer *c, TelsId dir, const char *name,
                        TelsId *out_id, uint64_t *out_gen) {
    uint32_t n = tels_list(c, dir, NULL, 0);
    if (n == 0) return FALSE;
    TelsId *ids = (TelsId *)malloc(sizeof(TelsId) * n);
    if (!ids) return FALSE;
    n = tels_list(c, dir, ids, n);
    BOOL found = FALSE;
    uint64_t best = 0;
    for (uint32_t i = 0; i < n; i++) {
        char en[TELS_MAX_NAME + 1];
        TelsType t = (TelsType)0;
        TelsId p = 0;
        uint64_t g = 0;
        if (!tels_info(c, ids[i], &t, &p, en, sizeof(en))) continue;
        if (t != TELS_TYPE_FILE || !parse_gen(en, name, &g)) continue;
        if (!found || g > best) {
            found = TRUE;
            best = g;
            *out_id = ids[i];
        }
    }
    free(ids);
    if (found && out_gen) *out_gen = best;
    return found;
}

/* همه‌ی نسل‌های name به‌جز keep را حذف می‌کند؛ keep = UINT64_MAX یعنی همه را */
static void delete_gens_except(TelsContainer *c, TelsId dir, const char *name, uint64_t keep) {
    uint32_t n = tels_list(c, dir, NULL, 0);
    if (n == 0) return;
    TelsId *ids = (TelsId *)malloc(sizeof(TelsId) * n);
    if (!ids) return;
    n = tels_list(c, dir, ids, n);
    for (uint32_t i = 0; i < n; i++) {
        char en[TELS_MAX_NAME + 1];
        TelsType t = (TelsType)0;
        TelsId p = 0;
        uint64_t g = 0;
        if (!tels_info(c, ids[i], &t, &p, en, sizeof(en))) continue;
        if (parse_gen(en, name, &g) && g != keep)
            tels_delete(c, ids[i]);
    }
    free(ids);
}

/* ───── compaction ─────
   lass.tels فقط‌افزودنی است و داده‌ی قدیمی را نگه می‌دارد. وقتی فایل بزرگ شد و بیشترش
   بلااستفاده بود، داده‌ی زنده در فایل موقت <lass.tels>.compact کپی می‌شود و همان فایل با
   MoveFileEx جای lass.tels را می‌گیرد. اگر وسط کار کرش شود lass.tels دست‌نخورده است و
   فایل موقت در بازشدن بعدی حذف می‌شود. */

#define STORE_COMPACT_SUFFIX ".compact"
#define STORE_REPLACE_TRIES  10
#define STORE_REPLACE_WAIT   100u

/* مسیر فایل store را برای compaction نگه می‌دارد؛ مسیر خیلی بلند یعنی compaction غیرفعال */
static void store_set_path(const char *path) {
    size_t n = strlen(path);
    if (n + sizeof(STORE_COMPACT_SUFFIX) > sizeof(g_store_path)) {
        g_store_path[0] = '\0';
        return;
    }
    memcpy(g_store_path, path, n + 1u);
}

/* نام فایل موقت compaction را در buf می‌نویسد */
static void store_temp_path(char *buf, size_t sz) {
    snprintf(buf, sz, "%s%s", g_store_path, STORE_COMPACT_SUFFIX);
}

static uint64_t store_file_bytes(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | (uint64_t)a.nFileSizeLow;
}

/* فایل موقت باقی‌مانده از compaction کرش‌کرده را حذف می‌کند */
static void store_remove_stale_temp(void) {
    if (!g_store_path[0]) return;
    char tmp[sizeof(g_store_path) + sizeof(STORE_COMPACT_SUFFIX)];
    store_temp_path(tmp, sizeof(tmp));
    if (GetFileAttributesA(tmp) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(tmp);
        LOGW(LOG_MOD_CORE, "store: removed a temporary file left by an interrupted compaction");
    }
}

/* g_store_lock باید گرفته شده باشد. اگر فایل نیاز داشته باشد بازنویسی می‌کند.
   خطاها فقط لاگ می‌شوند؛ در آن صورت فایل قبلی سر جایش می‌ماند و store کار می‌کند. */
static void store_maybe_compact_locked(void) {
    if (!g_store || !g_store_path[0] || !tels_should_compact(g_store)) return;

    char tmp[sizeof(g_store_path) + sizeof(STORE_COMPACT_SUFFIX)];
    store_temp_path(tmp, sizeof(tmp));
    uint64_t before = store_file_bytes(g_store_path);

    DeleteFileA(tmp);
    if (!tels_compact_to(g_store, tmp)) {
        LOGW(LOG_MOD_CORE, "store: compaction failed, the store file is unchanged");
        return;
    }

    /* محتوای فایل قبلاً commit شده؛ حالا می‌بندیم تا جایگزینی ممکن شود */
    tels_close(g_store);
    g_store = NULL;

    BOOL moved = FALSE;
    for (int i = 0; i < STORE_REPLACE_TRIES && !moved; i++) {
        moved = MoveFileExA(tmp, g_store_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!moved) Sleep(STORE_REPLACE_WAIT);
    }
    if (!moved) {
        DeleteFileA(tmp);
        LOGW(LOG_MOD_CORE, "store: compaction could not replace the file, keeping the old one");
    }

    g_store = tels_open(g_store_path, FALSE);
    if (!g_store) {
        LOGE(LOG_MOD_CORE, "store: cannot reopen %s after compaction", g_store_path);
        return;
    }
    if (moved) {
        LOGI(LOG_MOD_CORE, "store: compacted %s from %llu to %llu bytes", g_store_path,
             (unsigned long long)before, (unsigned long long)store_file_bytes(g_store_path));
    }
}

BOOL lass_store_open(const char *path) {
    if (!path || !path[0]) return FALSE;
    store_lock_init();
    EnterCriticalSection(&g_store_lock);
    if (g_store) {
        tels_close(g_store);
        g_store = NULL;
    }
    store_set_path(path);
    store_remove_stale_temp();
    g_store = tels_open(path, TRUE);
    store_maybe_compact_locked();
    BOOL ok = (g_store != NULL);
    LeaveCriticalSection(&g_store_lock);
    if (!ok) {
        LOGE(LOG_MOD_CORE, "store: cannot open %s (damaged, or written by another user or machine)", path);
        return FALSE;
    }
    LOGI(LOG_MOD_CORE, "store: opened %s", path);
    return TRUE;
}


BOOL lass_store_open_default(void) {
    char dir[MAX_PATH] = {0};
    char path[MAX_PATH] = {0};
    if (!lass_data_dir(dir, sizeof(dir)))   /* %APPDATA%\Lass، یا LASS_DATA_DIR در تست */
        return FALSE;
    CreateDirectoryA(dir, NULL);             /* اگر از قبل باشد، خطا مهم نیست */
    if (!path_join(path, sizeof(path), dir, "\\lass.tels"))
        return FALSE;
    return lass_store_open(path);
}

void lass_store_close(void) {
    store_lock_init();
    EnterCriticalSection(&g_store_lock);
    if (g_store) {
        tels_close(g_store);
        g_store = NULL;
    }
    LeaveCriticalSection(&g_store_lock);
}

BOOL lass_store_put(const char *dir, const char *name, const void *data, uint32_t len) {
    if (!dir || !name || (!data && len != 0)) return FALSE;
    size_t nl = strlen(name);
    if (nl == 0 || nl + 1 + STORE_GEN_DIGITS > TELS_MAX_NAME) return FALSE;

    store_lock_init();
    EnterCriticalSection(&g_store_lock);
    BOOL ok = FALSE;
    TelsContainer *c = g_store;
    if (c) {
        TelsId dir_id = 0, old_id = 0;
        uint64_t old_gen = 0;
        if (tels_resolve(c, dir, TRUE, &dir_id)) {
            BOOL had_old = find_newest(c, dir_id, name, &old_id, &old_gen);
            uint64_t gen = had_old ? old_gen + 1u : 1u;
            char entry[TELS_MAX_NAME + 1];
            snprintf(entry, sizeof(entry), "%s#%llu", name, (unsigned long long)gen);
            TelsId nid = 0;
            if (tels_create(c, dir_id, entry, TELS_TYPE_FILE, &nid)) {
                const uint8_t *src = (const uint8_t *)data;
                ok = TRUE;
                for (uint32_t off = 0; ok && off < len; ) {
                    uint32_t left = len - off;
                    uint32_t chunk = (left < STORE_CHUNK) ? left : STORE_CHUNK;
                    ok = tels_write(c, nid, off, src + off, chunk);
                    off += chunk;
                }
                if (ok) {
                    delete_gens_except(c, dir_id, name, gen);
                    ok = tels_commit(c);
                } else {
                    tels_delete(c, nid);
                }
            }
        }
    }
    if (ok) store_maybe_compact_locked();
    LeaveCriticalSection(&g_store_lock);
    return ok;
}

BOOL lass_store_get(const char *dir, const char *name, uint8_t **out, uint32_t *len) {
    if (!dir || !name || !out || !len) return FALSE;
    *out = NULL;
    *len = 0;
    store_lock_init();
    EnterCriticalSection(&g_store_lock);
    BOOL ok = FALSE;
    TelsContainer *c = g_store;
    if (c) {
        TelsId dir_id = 0, fid = 0;
        if (tels_resolve(c, dir, FALSE, &dir_id) && find_newest(c, dir_id, name, &fid, NULL)) {
            uint64_t size = 0;
            if (tels_size(c, fid, &size) && size <= 0xFFFFFFFFu) {
                uint8_t *buf = (uint8_t *)malloc(size ? (size_t)size : 1u);
                if (buf) {
                    ok = TRUE;
                    for (uint64_t off = 0; ok && off < size; ) {
                        uint64_t left = size - off;
                        uint32_t chunk = (left < STORE_CHUNK) ? (uint32_t)left : STORE_CHUNK;
                        ok = (tels_read(c, fid, off, buf + off, chunk) == (int32_t)chunk);
                        off += chunk;
                    }
                    if (ok) {
                        *out = buf;
                        *len = (uint32_t)size;
                    } else {
                        free(buf);
                    }
                }
            }
        }
    }
    LeaveCriticalSection(&g_store_lock);
    return ok;
}

BOOL lass_store_exists(const char *dir, const char *name) {
    if (!dir || !name) return FALSE;
    store_lock_init();
    EnterCriticalSection(&g_store_lock);
    BOOL found = FALSE;
    TelsContainer *c = g_store;
    if (c) {
        TelsId dir_id = 0, fid = 0;
        found = tels_resolve(c, dir, FALSE, &dir_id) && find_newest(c, dir_id, name, &fid, NULL);
    }
    LeaveCriticalSection(&g_store_lock);
    return found;
}

BOOL lass_store_delete(const char *dir, const char *name) {
    if (!dir || !name) return FALSE;
    store_lock_init();
    EnterCriticalSection(&g_store_lock);
    BOOL ok = FALSE;
    TelsContainer *c = g_store;
    if (c) {
        TelsId dir_id = 0;
        if (tels_resolve(c, dir, FALSE, &dir_id)) {
            delete_gens_except(c, dir_id, name, UINT64_MAX);
            ok = tels_commit(c);
        } else {
            ok = TRUE;   /* پوشه وجود ندارد یعنی چیزی برای حذف نیست */
        }
    }
    if (ok) store_maybe_compact_locked();
    LeaveCriticalSection(&g_store_lock);
    return ok;
}
