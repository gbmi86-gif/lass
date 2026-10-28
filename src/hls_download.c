/* hls_download.c — دانلود کامل جریان HLS با resume در سطح segment
   ─────────────────────────────────────────────────────────────
   طرح کانتینر موقت (فایل .tels دانلود):
     hls.meta    اثر انگشت playlist، تعداد segment ها و نام نهایی خروجی
     sNNNNNN     داده‌ی segment شماره‌ی NNNNNN (پس از رمزگشایی)
     dNNNNNN     نشانه‌ی کامل بودن همان segment (فایل صفر بایتی)
     mNN         داده‌ی بخش init شماره‌ی NN (EXT-X-MAP)
     mkNN        نشانه‌ی کامل بودن بخش init NN

   ترتیب نوشتن در هر segment: اول داده، بعد نشانه. اگر وسط کار قطع شود،
   نشانه نیست و همان segment دوباره گرفته می‌شود؛ هیچ segment ناقصی به‌عنوان
   کامل شناخته نمی‌شود. داده‌ی هر segment قبل از نوشتن کامل در حافظه است
   (سقف HLS_SEG_MAX).
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "hls_download.h"
#include "http.h"
#include "tels_container.h"
#include "lass_log.h"
#include "../plugins/plugin_hls.h"

#define HLS_PLAYLIST_MAX   (8u * 1024u * 1024u)
#define HLS_SEG_MAX        (64u * 1024u * 1024u)
#define HLS_KEY_LEN        16u
#define HLS_IO_CHUNK       (4u * 1024u * 1024u)
#define HLS_CONCAT_CHUNK   (1u * 1024u * 1024u)
#define HLS_READ_CHUNK     65536u
#define HLS_SEG_ATTEMPTS   4u
#define HLS_FETCH_TRIES    3u
#define HLS_COMMIT_EVERY   16u
#define HLS_BUSY_TRIES     30u
#define HLS_RETRY_CAP_SEC  60u
#define HLS_META_NAME      "hls.meta"

typedef struct {
    uint8_t *data;
    uint64_t len;
    uint64_t cap;
} HlsBuf;

typedef struct {
    uint8_t ok[HLS_MAX_KEYS];
    uint8_t val[HLS_MAX_KEYS][HLS_KEY_LEN];
} HlsKeyCache;

/* ======================================================
   کمک‌های اجرا
   ====================================================== */
/* کپی امن رشته به بافر با اندازه‌ی ثابت (بدون snprintf؛ رجوع کن به hls_copy_str در engine.c) */
static void str_copy(char *dst, size_t dst_sz, const char *src) {
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void run_error(HlsRun *run, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(run->error, sizeof(run->error), fmt, ap);
    va_end(ap);
    run->error[sizeof(run->error) - 1] = '\0';
}

static BOOL run_stop(const HlsRun *run) {
    return (run->should_stop && run->should_stop(run->stop_ctx)) ? TRUE : FALSE;
}

/* خواب کوتاه‌شده با بررسی توقف؛ FALSE یعنی متوقف شد */
static BOOL sleep_checked(const HlsRun *run, uint32_t ms) {
    uint32_t left = ms;
    while (left > 0) {
        if (run_stop(run)) return FALSE;
        uint32_t step = left > 100u ? 100u : left;
        Sleep(step);
        left -= step;
    }
    return !run_stop(run);
}

static void report(const HlsRun *run, uint64_t bytes, uint32_t done, uint32_t total) {
    if (run->on_progress) run->on_progress(bytes, done, total, run->progress_ctx);
}

static BOOL buf_reserve(HlsBuf *b, uint64_t need) {
    if (need <= b->cap) return TRUE;
    if (need > HLS_SEG_MAX) return FALSE;
    uint64_t cap = b->cap ? b->cap : (1024u * 1024u);
    while (cap < need) cap *= 2u;
    if (cap > HLS_SEG_MAX) cap = HLS_SEG_MAX;
    uint8_t *n = (uint8_t *)realloc(b->data, (size_t)cap);
    if (!n) return FALSE;
    b->data = n;
    b->cap = cap;
    return TRUE;
}

static void buf_free(HlsBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ======================================================
   HTTP: یک GET (با Range اختیاری) و خواندن کل بدنه
   ====================================================== */

/* کد خروجی در *status_out: ۰ خطای شبکه، -1 سرور Range را نادیده گرفت، -2 بدنه از سقف بزرگ‌تر است */
static HlsStatus http_fetch(HlsRun *run, const char *url, uint64_t start, uint64_t end,
                            uint64_t max_len, BOOL need_partial, HlsBuf *out,
                            int *status_out, uint32_t *retry_after) {
    out->len = 0;
    *status_out = 0;
    *retry_after = 0;
    int st = 0;
    uint32_t ra = 0;
    HTTP_Conn *conn = http_open_range_status(url, run->referer, run->dl,
                                             start, end, &ra, &st);
    if (!conn) {
        *status_out = st;
        *retry_after = ra;
        return HLS_ERR_FETCH;
    }
    if (need_partial && !http_conn_got_partial(conn)) {
        http_close(conn);
        *status_out = -1;
        return HLS_ERR_FETCH;
    }
    for (;;) {
        if (run_stop(run)) {
            http_close(conn);
            return HLS_ERR_STOPPED;
        }
        if (out->len >= max_len) {
            /* سقف رسیده؛ اگر هنوز داده هست بدنه بزرگ‌تر از حد است */
            uint8_t probe = 0;
            int pn = http_read(conn, &probe, 1);
            http_close(conn);
            if (pn > 0) {
                *status_out = -2;
                return HLS_ERR_IO;
            }
            if (pn < 0) return HLS_ERR_FETCH;
            return HLS_OK;
        }
        uint64_t room = max_len - out->len;
        uint32_t want = room > HLS_READ_CHUNK ? HLS_READ_CHUNK : (uint32_t)room;
        if (!buf_reserve(out, out->len + want)) {
            http_close(conn);
            *status_out = -2;
            return HLS_ERR_IO;
        }
        int n = http_read(conn, out->data + out->len, (int)want);
        if (n < 0) {
            http_close(conn);
            return HLS_ERR_FETCH;
        }
        if (n == 0) {
            http_close(conn);
            return HLS_OK;
        }
        out->len += (uint64_t)n;
    }
}

/* همان GET با تلاش مجدد: 429/503 با Retry-After، خطای شبکه با ۱ و ۲ و ۴ ثانیه؛
   404/401/403/410 و بقیه‌ی 4xx دائمی هستند و دوباره امتحان نمی‌شوند. */
static HlsStatus fetch_retry(HlsRun *run, const char *url, uint64_t start, uint64_t end,
                             uint64_t max_len, BOOL need_partial, uint32_t attempts,
                             HlsBuf *out, const char *what) {
    int last = 0;
    for (uint32_t a = 1; a <= attempts; a++) {
        int st = 0;
        uint32_t ra = 0;
        HlsStatus r = http_fetch(run, url, start, end, max_len, need_partial, out, &st, &ra);
        if (r == HLS_OK) return HLS_OK;
        if (r == HLS_ERR_STOPPED) return r;
        last = st;
        if (st == -1) {
            run_error(run, "%s: the server ignored the byte range request", what);
            return HLS_ERR_FETCH;
        }
        if (st == -2) {
            run_error(run, "%s is larger than the supported size", what);
            return HLS_ERR_FETCH;
        }
        if (st == 401 || st == 403 || st == 404 || st == 410) {
            run_error(run, "%s: HTTP %d", what, st);
            return HLS_ERR_NOT_FOUND;
        }
        if (st >= 400 && st < 500 && st != 408 && st != 429) {
            run_error(run, "%s: HTTP %d", what, st);
            return HLS_ERR_FETCH;
        }
        if (a == attempts) break;
        uint32_t wait_s = (ra > 0)
            ? (ra > HLS_RETRY_CAP_SEC ? HLS_RETRY_CAP_SEC : ra)
            : (1u << (a - 1u));
        if (!sleep_checked(run, wait_s * 1000u)) return HLS_ERR_STOPPED;
    }
    if (last > 0)
        run_error(run, "%s: HTTP %d after %u attempts", what, last, (unsigned)attempts);
    else
        run_error(run, "%s: network error after %u attempts", what, (unsigned)attempts);
    return HLS_ERR_FETCH;
}

/* segment: کامل، یا زیربازه‌ی دقیق با Range (سرور باید 206 بدهد) */
static HlsStatus fetch_segment(HlsRun *run, const HLS_Playlist *pl, uint32_t i, HlsBuf *out) {
    const HLS_Segment *s = &pl->segments[i];
    char what[48];
    snprintf(what, sizeof(what), "segment #%u", i + 1u);
    if (!s->has_range)
        return fetch_retry(run, s->url, 0, 0, HLS_SEG_MAX, FALSE,
                           HLS_SEG_ATTEMPTS, out, what);
    HlsStatus st = fetch_retry(run, s->url, s->range_off,
                               s->range_off + s->range_len - 1u, s->range_len, TRUE,
                               HLS_SEG_ATTEMPTS, out, what);
    if (st == HLS_OK && out->len != s->range_len) {
        run_error(run, "%s: the byte range returned %llu bytes, expected %llu",
                  what, (unsigned long long)out->len, (unsigned long long)s->range_len);
        return HLS_ERR_FETCH;
    }
    return st;
}

/* کلید AES-128 (۱۶ بایت) یک بار گرفته و در کش نگه داشته می‌شود */
static HlsStatus key_get(HlsRun *run, const HLS_Playlist *pl, int kidx,
                         HlsKeyCache *kc, uint8_t out[HLS_KEY_LEN]) {
    if (!kc->ok[kidx]) {
        HlsBuf kb;
        memset(&kb, 0, sizeof(kb));
        char what[48];
        snprintf(what, sizeof(what), "encryption key %d", kidx + 1);
        HlsStatus st = fetch_retry(run, pl->keys[kidx].uri, 0, 0, 64u, FALSE,
                                   HLS_FETCH_TRIES, &kb, what);
        if (st != HLS_OK) {
            buf_free(&kb);
            return st;
        }
        if (kb.len != HLS_KEY_LEN) {
            run_error(run, "%s has %llu bytes, expected 16", what,
                      (unsigned long long)kb.len);
            buf_free(&kb);
            return HLS_ERR_CRYPTO;
        }
        memcpy(kc->val[kidx], kb.data, HLS_KEY_LEN);
        kc->ok[kidx] = 1;
        buf_free(&kb);
    }
    memcpy(out, kc->val[kidx], HLS_KEY_LEN);
    return HLS_OK;
}

/* IV: صریح از EXT-X-KEY، یا شماره‌ی توالی رسانه به‌صورت عدد ۱۲۸ بیتی big-endian */
static void seg_iv(const HLS_Playlist *pl, const HLS_Segment *seg, uint8_t iv[16]) {
    const HLS_Key *k = &pl->keys[seg->key_index];
    if (k->has_iv) {
        memcpy(iv, k->iv, 16);
        return;
    }
    memset(iv, 0, 16);
    for (int i = 0; i < 8; i++)
        iv[15 - i] = (uint8_t)(seg->media_seq >> (8 * i));
}

/* AES-128-CBC با padding از نوع PKCS#7؛ در buf جایگزین می‌شود */
static HlsStatus aes128_cbc_decrypt(HlsRun *run, const uint8_t key[HLS_KEY_LEN],
                                    const uint8_t iv[16], HlsBuf *buf, uint32_t seg_no) {
    if (buf->len == 0 || (buf->len % 16u) != 0) {
        run_error(run, "segment #%u: encrypted size %llu is not a multiple of 16",
                  seg_no + 1u, (unsigned long long)buf->len);
        return HLS_ERR_CRYPTO;
    }
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE hk = NULL;
    uint8_t *plain = NULL;
    HlsStatus st = HLS_ERR_CRYPTO;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                         (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                         (ULONG)sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &hk, NULL, 0,
                                                  (PUCHAR)key, HLS_KEY_LEN, 0))) {
        plain = (uint8_t *)malloc((size_t)buf->len);
        if (plain) {
            uint8_t ivc[16];
            memcpy(ivc, iv, 16);
            ULONG outlen = 0;
            NTSTATUS ns = BCryptDecrypt(hk, buf->data, (ULONG)buf->len, NULL,
                                        ivc, 16, plain, (ULONG)buf->len, &outlen,
                                        BCRYPT_BLOCK_PADDING);
            SecureZeroMemory(ivc, sizeof(ivc));
            if (BCRYPT_SUCCESS(ns)) {
                free(buf->data);
                buf->data = plain;
                buf->cap = buf->len;     /* اندازه‌ی واقعی تخصیص همان طول قبلی است */
                buf->len = outlen;
                plain = NULL;
                st = HLS_OK;
            }
        }
    }
    free(plain);
    if (hk) BCryptDestroyKey(hk);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (st != HLS_OK)
        run_error(run, "segment #%u: decryption failed (wrong key or damaged data)",
                  seg_no + 1u);
    return st;
}

/* ======================================================
   اثر انگشت playlist و نام‌های کانتینر
   ====================================================== */
static uint64_t fnv_mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* بدون query: توکن‌های امضاشده‌ی URL در هر بار دریافت عوض می‌شوند */
static void strip_query(const char *url, char *out, size_t sz) {
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    if (n >= sz) n = sz - 1u;
    memcpy(out, url, n);
    out[n] = '\0';
}

/* اثر انگشت: شماره‌ی توالی، مسیر بدون query، مدت، و بازه‌های بایت هر segment */
static uint64_t playlist_fingerprint(const HLS_Playlist *pl) {
    uint64_t h = 1469598103934665603ULL;
    h = fnv_mix(h, &pl->media_sequence, sizeof(pl->media_sequence));
    for (int i = 0; i < pl->segment_count; i++) {
        const HLS_Segment *s = &pl->segments[i];
        char base[HLS_MAX_URL];
        strip_query(s->url, base, sizeof(base));
        h = fnv_mix(h, base, strlen(base));
        uint64_t dms = (uint64_t)(s->duration * 1000.0 + 0.5);
        h = fnv_mix(h, &dms, sizeof(dms));
        uint8_t hasr = s->has_range ? 1u : 0u;
        h = fnv_mix(h, &hasr, 1);
        uint64_t rng[2] = { s->range_off, s->range_len };
        h = fnv_mix(h, rng, sizeof(rng));
    }
    return h;
}

static void seg_names(uint32_t i, char sn[16], char dn[16]) {
    snprintf(sn, 16, "s%06u", (unsigned)i);
    snprintf(dn, 16, "d%06u", (unsigned)i);
}

static void map_names(int m, char mn[16], char mk[16]) {
    snprintf(mn, 16, "m%02d", m);
    snprintf(mk, 16, "mk%02d", m);
}

static BOOL path_exists(const char *p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* نام نهایی: پسوند playlist کنار گذاشته می‌شود؛ با EXT-X-MAP پسوند mp4، وگرنه ts؛
   اگر نام مشغول باشد عدد می‌گیرد: "name (1).ts" */
/* out = stem + ext، یا stem + " (n)" + ext. TRUE اگر در sz جا شد.
   stem قبلاً کوتاه شده است تا همیشه جا شود. */
static BOOL build_candidate(char *out, size_t sz, const char *stem, int n, const char *ext) {
    char num[16];
    num[0] = '\0';
    if (n > 0) snprintf(num, sizeof(num), " (%d)", n);
    size_t sl = strlen(stem), nl = strlen(num), el = strlen(ext);
    if (sl + nl + el + 1u > sz) return FALSE;
    memcpy(out, stem, sl);
    memcpy(out + sl, num, nl);
    memcpy(out + sl + nl, ext, el + 1u);
    return TRUE;
}

/* نام نهایی: پسوند playlist کنار گذاشته می‌شود؛ با EXT-X-MAP پسوند mp4، وگرنه ts؛
   اگر نام مشغول باشد عدد می‌گیرد: "name (1).ts" */
static void choose_final_path(const char *out_path, BOOL has_map, char *out, size_t sz) {
    char stem[IDM_MAX_PATH];
    str_copy(stem, sizeof(stem), out_path);
    const char *ext = has_map ? ".mp4" : ".ts";
    char *dot = strrchr(stem, '.');
    char *sep = strrchr(stem, '\\');
    char *sep2 = strrchr(stem, '/');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
    if (dot && (!sep || dot > sep)) {
        if (_stricmp(dot, ".m3u8") == 0 || _stricmp(dot, ".m3u") == 0 ||
            _stricmp(dot, ".ts") == 0 || _stricmp(dot, ".mp4") == 0 ||
            _stricmp(dot, ".m4s") == 0 || _stricmp(dot, ".m4v") == 0)
            *dot = '\0';
    }
    /* جای پسوند و شماره‌ی " (999)" باید باقی بماند */
    size_t room = sz > 32u ? sz - 32u : 0u;
    if (strlen(stem) > room) stem[room] = '\0';
    for (int n = 0; n < 1000; n++) {
        build_candidate(out, sz, stem, n, ext);
        if (!path_exists(out)) return;
    }
    build_candidate(out, sz, stem, 0, ext);
}

/* متن hls.meta — سه خط با پیشوند ثابت */
static int meta_format(char *buf, size_t sz, uint64_t fp, uint32_t segs, const char *final_name) {
    return snprintf(buf, sz, "lass-hls 1\nfp=%016llx\nsegments=%u\nfinal=%s\n",
                    (unsigned long long)fp, (unsigned)segs, final_name);
}

static BOOL meta_parse(const char *text, uint64_t *fp, uint32_t *segs,
                       char *final_name, size_t fn_sz) {
    if (strncmp(text, "lass-hls 1\n", 11) != 0) return FALSE;
    const char *f = strstr(text, "\nfp=");
    const char *s = strstr(text, "\nsegments=");
    const char *n = strstr(text, "\nfinal=");
    if (!f || !s || !n) return FALSE;
    *fp = (uint64_t)strtoull(f + 4, NULL, 16);
    *segs = (uint32_t)strtoul(s + 10, NULL, 10);
    const char *nb = n + 7;
    const char *ne = strchr(nb, '\n');
    size_t nl = ne ? (size_t)(ne - nb) : strlen(nb);
    if (nl == 0 || nl >= fn_sz) return FALSE;
    memcpy(final_name, nb, nl);
    final_name[nl] = '\0';
    return TRUE;
}

/* ======================================================
   کانتینر موقت
   ====================================================== */
static TelsId entry_find(TelsContainer *c, const char *name) {
    TelsId id = 0;
    return tels_lookup(c, TELS_ROOT, name, &id) ? id : 0;
}

static BOOL entry_exists(TelsContainer *c, const char *name) {
    return entry_find(c, name) != 0;
}

static BOOL entry_size_of(TelsContainer *c, const char *name, uint64_t *sz) {
    TelsId id = entry_find(c, name);
    return id != 0 && tels_size(c, id, sz);
}

static BOOL entry_drop(TelsContainer *c, const char *name) {
    TelsId id = entry_find(c, name);
    return id ? tels_delete(c, id) : TRUE;
}

/* جایگزین کردن کامل یک entry؛ اول حذف نسخه‌ی قبلی، بعد نوشتن در تکه‌های بزرگ */
static BOOL entry_put(TelsContainer *c, const char *name, const uint8_t *data, uint64_t len) {
    if (!entry_drop(c, name)) return FALSE;
    TelsId id = 0;
    if (!tels_create(c, TELS_ROOT, name, TELS_TYPE_FILE, &id)) return FALSE;
    for (uint64_t off = 0; off < len; ) {
        uint64_t left = len - off;
        uint32_t n = left > HLS_IO_CHUNK ? HLS_IO_CHUNK : (uint32_t)left;
        if (!tels_write(c, id, off, data + off, n)) return FALSE;
        off += n;
    }
    return TRUE;
}

/* نشانه‌ی کامل بودن: entry صفر بایتی */
static BOOL entry_marker(TelsContainer *c, const char *name) {
    if (!entry_drop(c, name)) return FALSE;
    TelsId id = 0;
    return tels_create(c, TELS_ROOT, name, TELS_TYPE_FILE, &id);
}

/* همه‌ی entry های کانتینر را حذف می‌کند (وقتی playlist عوض شده) */
static BOOL container_clear(TelsContainer *c) {
    uint32_t n = tels_list(c, TELS_ROOT, NULL, 0);
    if (n == 0) return TRUE;
    TelsId *ids = (TelsId *)malloc(sizeof(TelsId) * n);
    if (!ids) return FALSE;
    n = tels_list(c, TELS_ROOT, ids, n);
    BOOL ok = TRUE;
    for (uint32_t i = 0; i < n; i++)
        ok = tels_delete(c, ids[i]) && ok;
    free(ids);
    return ok;
}

/* داده‌ی entry را به فایل باز شده می‌نویسد */
static BOOL copy_entry(TelsContainer *c, HANDLE h, const char *name, uint8_t *buf) {
    TelsId id = entry_find(c, name);
    uint64_t size = 0;
    if (!id || !tels_size(c, id, &size)) return FALSE;
    for (uint64_t off = 0; off < size; ) {
        uint64_t left = size - off;
        uint32_t want = left > HLS_CONCAT_CHUNK ? HLS_CONCAT_CHUNK : (uint32_t)left;
        if (tels_read(c, id, off, buf, want) != (int32_t)want) return FALSE;
        DWORD w = 0;
        if (!WriteFile(h, buf, want, &w, NULL) || w != want) return FALSE;
        off += want;
    }
    return TRUE;
}

/* hls.meta از کانتینر؛ اگر نباشد یا خراب باشد FALSE */
static BOOL meta_load(TelsContainer *c, uint64_t *fp, uint32_t *segs,
                      char *final_name, size_t fn_sz) {
    TelsId id = entry_find(c, HLS_META_NAME);
    uint64_t sz = 0;
    if (!id || !tels_size(c, id, &sz) || sz == 0 || sz > 1024u) return FALSE;
    char text[1025];
    if (tels_read(c, id, 0, text, (uint32_t)sz) != (int32_t)sz) return FALSE;
    text[sz] = '\0';
    return meta_parse(text, fp, segs, final_name, fn_sz);
}

static void finish_container(TelsContainer *c) {
    if (c) {
        tels_commit(c);
        tels_close(c);
    }
}

/* کانتینر ممکن است هنوز توسط اجرای قبلی (در حال pause) باز باشد؛ چند بار تلاش می‌کنیم */
static TelsContainer *open_container(HlsRun *run) {
    for (uint32_t t = 0; t < HLS_BUSY_TRIES; t++) {
        TelsContainer *c = tels_open(run->temp_path, TRUE);
        if (c) return c;
        if (!sleep_checked(run, 1000u)) return NULL;
    }
    return NULL;
}

/* ======================================================
   playlist: گرفتن، انتخاب variant، و کنترل پشتیبانی
   ====================================================== */
static HlsStatus load_playlist(HlsRun *run, HLS_Playlist *pl) {
    HlsBuf b;
    memset(&b, 0, sizeof(b));
    HlsStatus st = fetch_retry(run, run->url, 0, 0, HLS_PLAYLIST_MAX, FALSE,
                               HLS_FETCH_TRIES, &b, "playlist");
    if (st != HLS_OK) {
        buf_free(&b);
        return st;
    }
    if (hls_parse_text(run->url, (const char *)b.data, (int)b.len, pl) != IDM_OK) {
        buf_free(&b);
        run_error(run, "the URL is not an HLS playlist (no #EXTM3U header)");
        return HLS_ERR_PARSE;
    }
    buf_free(&b);

    if (pl->is_master) {
        if (pl->variant_count == 0) {
            run_error(run, "master playlist has no quality variants");
            return HLS_ERR_PARSE;
        }
        const HLS_Variant *best = &pl->variants[pl->best_variant];
        if (best->audio_group[0] && pl->has_separate_audio) {
            run_error(run, "separate audio track (demuxed stream) is not supported yet");
            return HLS_ERR_UNSUPPORTED;
        }
        char best_url[HLS_MAX_URL];
        str_copy(best_url, sizeof(best_url), best->url);
        memset(pl, 0, sizeof(*pl));

        HlsBuf mb;
        memset(&mb, 0, sizeof(mb));
        st = fetch_retry(run, best_url, 0, 0, HLS_PLAYLIST_MAX, FALSE,
                         HLS_FETCH_TRIES, &mb, "media playlist");
        if (st != HLS_OK) {
            buf_free(&mb);
            return st;
        }
        if (hls_parse_text(best_url, (const char *)mb.data, (int)mb.len, pl) != IDM_OK) {
            buf_free(&mb);
            run_error(run, "the quality playlist is not a valid HLS media playlist");
            return HLS_ERR_PARSE;
        }
        buf_free(&mb);
        if (pl->is_master) {
            run_error(run, "nested master playlists are not supported");
            return HLS_ERR_UNSUPPORTED;
        }
    }

    if (pl->unsupported[0]) {
        run_error(run, "%s", pl->unsupported);
        return HLS_ERR_UNSUPPORTED;
    }
    if (pl->segment_count == 0) {
        run_error(run, "playlist has no media segments");
        return HLS_ERR_PARSE;
    }
    if (!pl->is_endlist) {
        run_error(run, "live playlist (no #EXT-X-ENDLIST): only complete streams can be downloaded");
        return HLS_ERR_UNSUPPORTED;
    }
    return HLS_OK;
}

/* ======================================================
   segment ها و بخش init: فقط آن‌هایی که نشانه‌شان نیست
   ====================================================== */
static HlsStatus ensure_map(HlsRun *run, const HLS_Playlist *pl, TelsContainer *c, int m) {
    char mn[16], mk[16];
    map_names(m, mn, mk);
    if (entry_exists(c, mk)) return HLS_OK;
    HlsBuf b;
    memset(&b, 0, sizeof(b));
    char what[48];
    snprintf(what, sizeof(what), "init section %d", m + 1);
    HlsStatus st = fetch_retry(run, pl->maps[m].uri, 0, 0, HLS_SEG_MAX, FALSE,
                               HLS_SEG_ATTEMPTS, &b, what);
    if (st == HLS_OK && (!entry_put(c, mn, b.data, b.len) || !entry_marker(c, mk))) {
        run_error(run, "cannot write init section %d to the temporary file", m + 1);
        st = HLS_ERR_IO;
    }
    buf_free(&b);
    return st;
}

static HlsStatus fetch_pending(HlsRun *run, const HLS_Playlist *pl, TelsContainer *c) {
    const uint32_t n = (uint32_t)pl->segment_count;
    HlsKeyCache *kc = (HlsKeyCache *)calloc(1, sizeof(HlsKeyCache));
    if (!kc) {
        run_error(run, "out of memory");
        return HLS_ERR_IO;
    }

    /* segment های کامل از قبل فقط برای گزارش پیشرفت شمرده می‌شوند */
    uint64_t bytes = 0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++) {
        char sn[16], dn[16];
        seg_names(i, sn, dn);
        if (!entry_exists(c, dn)) continue;
        uint64_t sz = 0;
        entry_size_of(c, sn, &sz);
        bytes += sz;
        done++;
    }
    report(run, bytes, done, n);

    uint32_t uncommitted = 0;
    for (uint32_t i = 0; i < n; i++) {
        char sn[16], dn[16];
        seg_names(i, sn, dn);
        if (entry_exists(c, dn)) continue;
        if (run_stop(run)) {
            free(kc);
            return HLS_ERR_STOPPED;
        }

        const HLS_Segment *seg = &pl->segments[i];
        HlsStatus st = HLS_OK;
        if (seg->map_index >= 0) st = ensure_map(run, pl, c, seg->map_index);

        HlsBuf body;
        memset(&body, 0, sizeof(body));
        if (st == HLS_OK) st = fetch_segment(run, pl, i, &body);
        if (st == HLS_OK && seg->key_index >= 0) {
            uint8_t key[HLS_KEY_LEN], iv[16];
            st = key_get(run, pl, seg->key_index, kc, key);
            if (st == HLS_OK) {
                seg_iv(pl, seg, iv);
                st = aes128_cbc_decrypt(run, key, iv, &body, i);
            }
            SecureZeroMemory(key, sizeof(key));
        }
        if (st != HLS_OK) {
            buf_free(&body);
            free(kc);
            return st;
        }

        if (!entry_put(c, sn, body.data, body.len) || !entry_marker(c, dn)) {
            buf_free(&body);
            free(kc);
            run_error(run, "cannot write segment #%u to the temporary file", i + 1u);
            return HLS_ERR_IO;
        }
        bytes += body.len;
        done++;
        buf_free(&body);
        if (++uncommitted >= HLS_COMMIT_EVERY) {
            tels_commit(c);
            uncommitted = 0;
        }
        report(run, bytes, done, n);
    }
    free(kc);
    return HLS_OK;
}

/* ======================================================
   فایل نهایی: segment ها به ترتیب (و بخش init وقتی لازم است)
   ====================================================== */
static HlsStatus write_output(HlsRun *run, const HLS_Playlist *pl, TelsContainer *c,
                              const char *final_name) {
    HANDLE h = CreateFileA(final_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        run_error(run, "cannot create the output file (error %lu)",
                  (unsigned long)GetLastError());
        return HLS_ERR_IO;
    }
    uint8_t *buf = (uint8_t *)malloc(HLS_CONCAT_CHUNK);
    if (!buf) {
        CloseHandle(h);
        run_error(run, "out of memory");
        return HLS_ERR_IO;
    }
    HlsStatus st = HLS_OK;
    int written_map = -1;
    for (uint32_t i = 0; i < (uint32_t)pl->segment_count; i++) {
        if (run_stop(run)) {
            st = HLS_ERR_STOPPED;
            break;
        }
        const HLS_Segment *seg = &pl->segments[i];
        char sn[16], dn[16];
        seg_names(i, sn, dn);
        if (seg->map_index >= 0 && seg->map_index != written_map) {
            char mn[16], mk[16];
            map_names(seg->map_index, mn, mk);
            if (!copy_entry(c, h, mn, buf)) {
                st = HLS_ERR_IO;
                break;
            }
            written_map = seg->map_index;
        }
        if (!copy_entry(c, h, sn, buf)) {
            st = HLS_ERR_IO;
            break;
        }
    }
    free(buf);
    if (st == HLS_OK && !FlushFileBuffers(h)) st = HLS_ERR_IO;
    CloseHandle(h);
    if (st == HLS_ERR_IO) run_error(run, "cannot write the output file");
    return st;
}

/* ======================================================
   یک دانلود کامل: کانتینر، ادامه یا شروع تازه، و فایل نهایی
   ====================================================== */
static HlsStatus download_stream(HlsRun *run, const HLS_Playlist *pl) {
    TelsContainer *c = open_container(run);
    if (!c) {
        if (run_stop(run)) return HLS_ERR_STOPPED;
        run_error(run, "temporary file is busy or damaged");
        return HLS_ERR_IO;
    }

    const uint64_t fp = playlist_fingerprint(pl);
    const uint32_t nseg = (uint32_t)pl->segment_count;
    uint64_t got_fp = 0;
    uint32_t got_segs = 0;
    char final_name[IDM_MAX_PATH];
    final_name[0] = '\0';
    BOOL resume = meta_load(c, &got_fp, &got_segs, final_name, sizeof(final_name)) &&
                  got_fp == fp && got_segs == nseg;

    if (!resume) {
        if (!container_clear(c)) {
            finish_container(c);
            run_error(run, "cannot reset the temporary file");
            return HLS_ERR_IO;
        }
        choose_final_path(run->out_path, pl->map_count > 0, final_name, sizeof(final_name));
        char text[512];
        int tl = meta_format(text, sizeof(text), fp, nseg, final_name);
        if (tl <= 0 || !entry_put(c, HLS_META_NAME, (const uint8_t *)text, (uint64_t)tl)) {
            finish_container(c);
            run_error(run, "cannot write the temporary file");
            return HLS_ERR_IO;
        }
        tels_commit(c);
    }

    HlsStatus st = fetch_pending(run, pl, c);
    if (st == HLS_OK) st = write_output(run, pl, c, final_name);
    if (st != HLS_OK) {
        finish_container(c);
        return st;
    }
    tels_close(c);
    if (!DeleteFileA(run->temp_path))
        LOGW(LOG_MOD_HLS, "temporary file %s could not be removed", run->temp_path);
    str_copy(run->final_path, sizeof(run->final_path), final_name);
    LOGI(LOG_MOD_HLS, "complete: %u segments written to %s", (unsigned)nseg, final_name);
    return HLS_OK;
}

/* ======================================================
   نقطه‌ی ورود
   ====================================================== */
BOOL hls_download_wants(const char *url) {
    return hls_is_m3u8_url(url);
}

HlsStatus hls_download_run(HlsRun *run) {
    if (!run || !run->url || !run->temp_path || !run->out_path) return HLS_ERR_IO;
    run->final_path[0] = '\0';
    run->error[0] = '\0';
    if (run_stop(run)) return HLS_ERR_STOPPED;

    HLS_Playlist *pl = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    if (!pl) {
        run_error(run, "out of memory");
        return HLS_ERR_IO;
    }
    HlsStatus st = load_playlist(run, pl);
    if (st == HLS_OK) st = download_stream(run, pl);
    free(pl);
    return st;
}
