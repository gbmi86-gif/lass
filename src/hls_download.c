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
#include "media_http.h"
#include "media_store.h"
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
#define HLS_META_HEAD      "lass-hls 1\n"

typedef MediaBuf HlsBuf;

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

/* ── HTTP و بافر مشترک (media_http.c) ─────────────────────────────── */
static MediaHttp hls_http(const HlsRun *run) {
    MediaHttp h;
    h.dl = run->dl;
    h.referer = run->referer;
    h.should_stop = run->should_stop;
    h.stop_ctx = run->stop_ctx;
    return h;
}

static BOOL run_stop(const HlsRun *run) {
    MediaHttp h = hls_http(run);
    return media_stopped(&h);
}

static void buf_free(HlsBuf *b) {
    media_buf_free(b);
}




static void report(const HlsRun *run, uint64_t bytes, uint32_t done, uint32_t total) {
    if (run->on_progress) run->on_progress(bytes, done, total, run->progress_ctx);
}



/* ======================================================
   HTTP: یک GET (با Range اختیاری) و خواندن کل بدنه
   ====================================================== */


/* 429/503 با Retry-After، خطای شبکه با ۱ و ۲ و ۴ ثانیه؛ 400/401/404/410 دائمی‌اند */
static HlsStatus fetch_retry(HlsRun *run, const char *url, uint64_t start, uint64_t end,
                             uint64_t max_len, BOOL need_partial, uint32_t attempts,
                             HlsBuf *out, const char *what) {
    MediaHttp h = hls_http(run);
    char err[sizeof(run->error)];
    MediaResult r = media_fetch(&h, url, start, end, max_len, need_partial, attempts,
                                out, what, err, sizeof(err));
    if (r == MEDIA_OK) return HLS_OK;
    if (r == MEDIA_STOPPED) return HLS_ERR_STOPPED;
    run_error(run, "%s", err);
    return r == MEDIA_NOT_FOUND ? HLS_ERR_NOT_FOUND : HLS_ERR_FETCH;
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




static BOOL entry_exists(TelsContainer *c, const char *name) {
    return media_entry_exists(c, name);
}

static BOOL entry_size_of(TelsContainer *c, const char *name, uint64_t *sz) {
    return media_entry_size(c, name, sz);
}

static BOOL entry_put(TelsContainer *c, const char *name, const uint8_t *data, uint64_t len) {
    return media_entry_put(c, name, data, len, HLS_IO_CHUNK);
}

static BOOL entry_marker(TelsContainer *c, const char *name) {
    return media_entry_marker(c, name);
}

static BOOL container_clear(TelsContainer *c) {
    return media_container_clear(c);
}

static BOOL copy_entry(TelsContainer *c, HANDLE h, const char *name, uint8_t *buf) {
    return media_entry_copy_to_handle(c, name, h, buf, HLS_CONCAT_CHUNK);
}

static void finish_container(TelsContainer *c) {
    media_container_finish(c);
}

static TelsContainer *open_container(HlsRun *run) {
    MediaHttp h = hls_http(run);
    return media_container_open(run->temp_path, HLS_BUSY_TRIES, &h);
}

static const char *const HLS_FINAL_STRIP[] = { ".m3u8", ".m3u", ".ts", ".mp4", ".m4s", ".m4v" };

static void choose_final_path(const char *out_path, BOOL has_map, char *out, size_t sz) {
    media_choose_final_path(out_path, has_map ? ".mp4" : ".ts", HLS_FINAL_STRIP,
                            sizeof(HLS_FINAL_STRIP) / sizeof(HLS_FINAL_STRIP[0]), out, sz);
}

static uint64_t fnv_mix(uint64_t h, const void *p, size_t n) {
    return media_fnv_mix(h, p, n);
}

static void strip_query(const char *url, char *out, size_t sz) {
    media_strip_query(url, out, sz);
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




/* ── قالب hls.meta: نوشتن و خواندن در media_store.c ─────────────── */
static int meta_format(char *buf, size_t sz, uint64_t fp, uint32_t segs, const char *final_name) {
    return media_meta_format(buf, sz, HLS_META_HEAD, fp, TRUE, segs, final_name);
}

static BOOL meta_load(TelsContainer *c, uint64_t *fp, uint32_t *segs, char *final_name, size_t fn_sz) {
    return media_meta_load(c, HLS_META_NAME, HLS_META_HEAD, TRUE, fp, segs, final_name, fn_sz);
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
