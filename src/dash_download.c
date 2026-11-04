/* dash_download.c — دانلود کامل DASH با resume در سطح segment و mux به MP4
   ─────────────────────────────────────────────────────────────
   - manifest با plugin_dash تجزیه می‌شود؛ بهترین video و audio انتخاب می‌شوند
   - init و هر segment روی کانتینر موقت نوشته و نشانه‌ی کامل بودن می‌گیرند
   - اجرای بعدی فقط segment های ناکامل را می‌گیرد (همان روش HLS)
   - وقتی همه کامل شد، قطعات با mp4_mux به یک MP4 با moov جلو تبدیل می‌شوند
   - خطا یا توقف: کانتینر می‌ماند؛ فایل ناقص نهایی حذف می‌شود
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "dash_download.h"
#include "http.h"
#include "tels_container.h"
#include "lass_log.h"
#include "mp4_mux.h"
#include "../plugins/plugin_dash.h"

#define DASH_MPD_MAX        (8u * 1024u * 1024u)
#define DASH_INIT_MAX       (32u * 1024u * 1024u)
#define DASH_SEG_MAX        (128u * 1024u * 1024u)
#define DASH_READ_CHUNK     (64u * 1024u)
#define DASH_IO_CHUNK       (256u * 1024u)
#define DASH_OUT_BUF        (1024u * 1024u)
#define DASH_FETCH_TRIES    3u
#define DASH_RETRY_CAP_SEC  60u
#define DASH_BUSY_TRIES     5u
#define DASH_COMMIT_EVERY   8u
#define DASH_MAX_SAMPLES    4000000u
#define DASH_META_NAME      "dash.meta"
#define DASH_META_HEAD      "lass-dash 1\n"
#define DASH_FNV_BASIS      1469598103934665603ULL
#define DASH_FNV_PRIME      1099511628211ULL

/* ======================================================
   بافر و کمک‌های عمومی
   ====================================================== */
typedef struct {
    uint8_t *data;
    uint64_t len;
    uint64_t cap;
} DashBuf;

static void buf_free(DashBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static BOOL buf_reserve(DashBuf *b, uint64_t need, uint64_t max) {
    uint64_t cap;
    uint8_t *n;
    if (need <= b->cap) return TRUE;
    if (need > max) return FALSE;
    cap = b->cap ? b->cap : (1024u * 1024u);
    while (cap < need) cap *= 2u;
    if (cap > max) cap = max;
    n = (uint8_t *)realloc(b->data, (size_t)cap);
    if (!n) return FALSE;
    b->data = n;
    b->cap = cap;
    return TRUE;
}

static void str_copy(char *dst, size_t dst_sz, const char *src) {
    size_t n;
    if (dst_sz == 0) return;
    n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void run_error(DashRun *run, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(run->error, sizeof(run->error), fmt, ap);
    va_end(ap);
    run->error[sizeof(run->error) - 1] = '\0';
}

static BOOL run_stop(const DashRun *run) {
    return (run->should_stop && run->should_stop(run->stop_ctx)) ? TRUE : FALSE;
}

/* خواب کوتاه‌شده با بررسی توقف؛ FALSE یعنی متوقف شد */
static BOOL sleep_checked(const DashRun *run, uint32_t ms) {
    uint32_t left = ms;
    while (left > 0) {
        uint32_t step;
        if (run_stop(run)) return FALSE;
        step = left > 100u ? 100u : left;
        Sleep(step);
        left -= step;
    }
    return !run_stop(run);
}

static void report(const DashRun *run, uint64_t bytes, uint32_t done, uint32_t total) {
    if (run->on_progress) run->on_progress(bytes, done, total, run->progress_ctx);
}

/* ======================================================
   HTTP: یک GET کامل و تلاش مجدد با همان قواعد HLS
   ====================================================== */
static DashStatus http_fetch(DashRun *run, const char *url, uint64_t max_len, DashBuf *out,
                             int *status_out, uint32_t *retry_after) {
    uint32_t ra = 0;
    int st = 0;
    HTTP_Conn *conn;
    out->len = 0;
    *status_out = 0;
    *retry_after = 0;
    conn = http_open_range_status(url, run->referer, run->dl, 0, 0, &ra, &st);
    if (!conn) {
        *status_out = st;
        *retry_after = ra;
        return DASH_ERR_FETCH;
    }
    for (;;) {
        uint64_t room;
        uint32_t want;
        int n;
        if (run_stop(run)) {
            http_close(conn);
            return DASH_ERR_STOPPED;
        }
        if (out->len >= max_len) {
            uint8_t probe = 0;
            int pn = http_read(conn, &probe, 1);
            http_close(conn);
            if (pn > 0) {
                *status_out = -2;   /* بدنه از سقف بزرگ‌تر است */
                return DASH_ERR_IO;
            }
            return pn < 0 ? DASH_ERR_FETCH : DASH_OK;
        }
        room = max_len - out->len;
        want = room > DASH_READ_CHUNK ? DASH_READ_CHUNK : (uint32_t)room;
        if (!buf_reserve(out, out->len + want, max_len)) {
            http_close(conn);
            *status_out = -2;
            return DASH_ERR_IO;
        }
        n = http_read(conn, out->data + out->len, (int)want);
        if (n < 0) {
            http_close(conn);
            return DASH_ERR_FETCH;
        }
        if (n == 0) {
            http_close(conn);
            return DASH_OK;
        }
        out->len += (uint64_t)n;
    }
}

/* 429/503 با Retry-After، خطای شبکه با ۱ و ۲ و ۴ ثانیه؛ 401/403/404/410 دائمی‌اند */
static DashStatus fetch_retry(DashRun *run, const char *url, uint64_t max_len, DashBuf *out,
                              const char *what) {
    int last = 0;
    uint32_t a;
    for (a = 1; a <= DASH_FETCH_TRIES; a++) {
        int st = 0;
        uint32_t ra = 0, wait_s;
        DashStatus r = http_fetch(run, url, max_len, out, &st, &ra);
        if (r == DASH_OK) return DASH_OK;
        if (r == DASH_ERR_STOPPED) return r;
        last = st;
        if (st == -2) {
            run_error(run, "%s is larger than the supported size", what);
            return DASH_ERR_FETCH;
        }
        if (st == 401 || st == 403 || st == 404 || st == 410) {
            run_error(run, "%s: HTTP %d", what, st);
            return DASH_ERR_NOT_FOUND;
        }
        if (st >= 400 && st < 500 && st != 408 && st != 429) {
            run_error(run, "%s: HTTP %d", what, st);
            return DASH_ERR_FETCH;
        }
        if (a == DASH_FETCH_TRIES) break;
        wait_s = (ra > 0) ? (ra > DASH_RETRY_CAP_SEC ? DASH_RETRY_CAP_SEC : ra) : (1u << (a - 1u));
        if (!sleep_checked(run, wait_s * 1000u)) return DASH_ERR_STOPPED;
    }
    if (last > 0)
        run_error(run, "%s: HTTP %d after %u attempts", what, last, (unsigned)DASH_FETCH_TRIES);
    else
        run_error(run, "%s: network error after %u attempts", what, (unsigned)DASH_FETCH_TRIES);
    return DASH_ERR_FETCH;
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

static BOOL entry_size_of_name(TelsContainer *c, const char *name, uint64_t *sz) {
    TelsId id = entry_find(c, name);
    return id != 0 && tels_size(c, id, sz);
}

static BOOL entry_drop(TelsContainer *c, const char *name) {
    TelsId id = entry_find(c, name);
    return id ? tels_delete(c, id) : TRUE;
}

/* جایگزین کامل یک entry؛ اول نسخه‌ی قبلی حذف، بعد نوشتن در تکه‌ها */
static BOOL entry_put(TelsContainer *c, const char *name, const uint8_t *data, uint64_t len) {
    TelsId id = 0;
    uint64_t off = 0;
    if (!entry_drop(c, name)) return FALSE;
    if (!tels_create(c, TELS_ROOT, name, TELS_TYPE_FILE, &id)) return FALSE;
    while (off < len) {
        uint64_t left = len - off;
        uint32_t n = left > DASH_IO_CHUNK ? DASH_IO_CHUNK : (uint32_t)left;
        if (!tels_write(c, id, off, data + off, n)) return FALSE;
        off += n;
    }
    return TRUE;
}

/* نشانه‌ی کامل بودن: entry صفر بایتی */
static BOOL entry_marker(TelsContainer *c, const char *name) {
    TelsId id = 0;
    if (!entry_drop(c, name)) return FALSE;
    return tels_create(c, TELS_ROOT, name, TELS_TYPE_FILE, &id);
}

static BOOL container_clear(TelsContainer *c) {
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

/* داده‌ی کامل یک entry در بافر؛ سقف max_len */
static BOOL read_entry(TelsContainer *c, const char *name, DashBuf *out, uint64_t max_len) {
    TelsId id = entry_find(c, name);
    uint64_t size = 0, off = 0;
    out->len = 0;
    if (!id || !tels_size(c, id, &size)) return FALSE;
    if (size > max_len || !buf_reserve(out, size, max_len)) return FALSE;
    while (off < size) {
        uint64_t left = size - off;
        uint32_t want = left > DASH_IO_CHUNK ? DASH_IO_CHUNK : (uint32_t)left;
        if (tels_read(c, id, off, out->data + off, want) != (int32_t)want) return FALSE;
        off += want;
    }
    out->len = size;
    return TRUE;
}

static void finish_container(TelsContainer *c) {
    if (c) {
        tels_commit(c);
        tels_close(c);
    }
}

/* کانتینر ممکن است هنوز در اجرای قبلی (در حال pause) باز باشد؛ چند بار تلاش */
static TelsContainer *open_container(DashRun *run) {
    uint32_t t;
    for (t = 0; t < DASH_BUSY_TRIES; t++) {
        TelsContainer *c = tels_open(run->temp_path, TRUE);
        if (c) return c;
        if (!sleep_checked(run, 1000u)) return NULL;
    }
    return NULL;
}

static void src_names(char tag, uint32_t i, char sn[16], char dn[16]) {
    snprintf(sn, 16, "%c%06u", tag, (unsigned)i);
    snprintf(dn, 16, "%cd%06u", tag, (unsigned)i);
}

static void init_names(char tag, char in_name[16], char mk_name[16]) {
    snprintf(in_name, 16, "%cinit", tag);
    snprintf(mk_name, 16, "%cinitd", tag);
}

/* متن dash.meta: هویت دانلود (fingerprint) و نام نهایی فایل */
static int meta_format(char *buf, size_t sz, uint64_t fp, const char *final_name) {
    return snprintf(buf, sz, "%sfp=%016llx\nfinal=%s\n", DASH_META_HEAD,
                    (unsigned long long)fp, final_name);
}

static BOOL meta_parse(const char *text, uint64_t *fp, char *final_name, size_t fn_sz) {
    const char *f, *n, *nb, *ne;
    size_t nl;
    if (strncmp(text, DASH_META_HEAD, strlen(DASH_META_HEAD)) != 0) return FALSE;
    f = strstr(text, "\nfp=");
    n = strstr(text, "\nfinal=");
    if (!f || !n) return FALSE;
    *fp = (uint64_t)strtoull(f + 4, NULL, 16);
    nb = n + 7;
    ne = strchr(nb, '\n');
    nl = ne ? (size_t)(ne - nb) : strlen(nb);
    if (nl == 0 || nl >= fn_sz) return FALSE;
    memcpy(final_name, nb, nl);
    final_name[nl] = '\0';
    return TRUE;
}

static BOOL meta_load(TelsContainer *c, uint64_t *fp, char *final_name, size_t fn_sz) {
    TelsId id = entry_find(c, DASH_META_NAME);
    uint64_t sz = 0;
    char text[1025];
    if (!id || !tels_size(c, id, &sz) || sz == 0 || sz > 1024u) return FALSE;
    if (tels_read(c, id, 0, text, (uint32_t)sz) != (int32_t)sz) return FALSE;
    text[sz] = '\0';
    return meta_parse(text, fp, final_name, fn_sz);
}

/* ======================================================
   نام نهایی: پسوند منبع کنار گذاشته، پسوند mp4؛ اگر مشغول بود عدد می‌گیرد
   ====================================================== */
static BOOL path_exists(const char *p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static BOOL build_candidate(char *out, size_t sz, const char *stem, int n) {
    char num[16];
    size_t sl, nl, el;
    num[0] = '\0';
    if (n > 0) snprintf(num, sizeof(num), " (%d)", n);
    sl = strlen(stem);
    nl = strlen(num);
    el = strlen(".mp4");
    if (sl + nl + el + 1u > sz) return FALSE;
    memcpy(out, stem, sl);
    memcpy(out + sl, num, nl);
    memcpy(out + sl + nl, ".mp4", el + 1u);
    return TRUE;
}

static void choose_final_path(const char *out_path, char *out, size_t sz) {
    char stem[IDM_MAX_PATH];
    char *dot, *sep, *sep2;
    size_t room;
    int n;
    str_copy(stem, sizeof(stem), out_path);
    dot = strrchr(stem, '.');
    sep = strrchr(stem, '\\');
    sep2 = strrchr(stem, '/');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
    if (dot && (!sep || dot > sep)) {
        if (_stricmp(dot, ".mpd") == 0 || _stricmp(dot, ".mp4") == 0 ||
            _stricmp(dot, ".m4s") == 0 || _stricmp(dot, ".m4v") == 0 ||
            _stricmp(dot, ".ts") == 0)
            *dot = '\0';
    }
    room = sz > 32u ? sz - 32u : 0u;
    if (strlen(stem) > room) stem[room] = '\0';
    for (n = 0; n < 1000; n++) {
        if (build_candidate(out, sz, stem, n) && !path_exists(out)) return;
    }
    build_candidate(out, sz, stem, 0);
}

/* ======================================================
   fingerprint: هویت یک دانلود از روی representation ها و آدرس‌ها (بدون query)
   ====================================================== */
static uint64_t fnv_mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= DASH_FNV_PRIME;
    }
    return h;
}

static void strip_query(const char *url, char *out, size_t sz) {
    size_t n = 0;
    if (sz == 0) return;
    while (url[n] && url[n] != '?' && n + 1 < sz) {
        out[n] = url[n];
        n++;
    }
    out[n] = '\0';
}

typedef struct {
    const DASH_Representation *rep;
    char     tag;          /* 'v' یا 'a' */
    uint32_t count;        /* تعداد segment ها */
    Mp4Init  init;         /* بعد از خواندن init */
    DashBuf  init_buf;     /* بایت init؛ اشاره‌گرهای init به آن هستند */
} DashSource;

static BOOL fingerprint_source(const DashSource *s, uint64_t *h) {
    char url[DASH_MAX_URL], clean[DASH_MAX_URL];
    uint32_t i;
    *h = fnv_mix(*h, &s->tag, 1);
    *h = fnv_mix(*h, s->rep->id, strlen(s->rep->id));
    *h = fnv_mix(*h, &s->rep->bandwidth, sizeof(s->rep->bandwidth));
    *h = fnv_mix(*h, &s->count, sizeof(s->count));
    strip_query(s->rep->init_url, clean, sizeof(clean));
    *h = fnv_mix(*h, clean, strlen(clean));
    for (i = 0; i < s->count; i++) {
        if (!dash_rep_segment_url(s->rep, i, url, sizeof(url))) return FALSE;
        strip_query(url, clean, sizeof(clean));
        *h = fnv_mix(*h, clean, strlen(clean));
    }
    return TRUE;
}

/* ======================================================
   دانلود init و segment ها با resume
   ====================================================== */
static DashStatus fetch_pending(DashRun *run, const DashSource *src, int nsrc, TelsContainer *c) {
    uint64_t bytes = 0;
    uint32_t done = 0, total = 0, uncommitted = 0;
    int s;
    for (s = 0; s < nsrc; s++) total += src[s].count + 1u;

    for (s = 0; s < nsrc; s++) {
        char in_name[16], mk_name[16], sn[16], dn[16];
        uint32_t i;
        init_names(src[s].tag, in_name, mk_name);
        if (entry_exists(c, mk_name)) {
            uint64_t sz = 0;
            TelsId id = entry_find(c, in_name);
            if (id && tels_size(c, id, &sz)) bytes += sz;
            done++;
        }
        for (i = 0; i < src[s].count; i++) {
            uint64_t sz = 0;
            src_names(src[s].tag, i, sn, dn);
            if (!entry_exists(c, dn)) continue;
            if (entry_size_of_name(c, sn, &sz)) bytes += sz;
            done++;
        }
    }
    report(run, bytes, done, total);

    for (s = 0; s < nsrc; s++) {
        char in_name[16], mk_name[16];
        DashBuf body;
        DashStatus st;
        const char *what = src[s].tag == 'v' ? "video initialization segment"
                                             : "audio initialization segment";
        init_names(src[s].tag, in_name, mk_name);
        if (entry_exists(c, mk_name)) continue;
        if (run_stop(run)) return DASH_ERR_STOPPED;
        memset(&body, 0, sizeof(body));
        st = fetch_retry(run, src[s].rep->init_url, DASH_INIT_MAX, &body, what);
        if (st != DASH_OK) {
            buf_free(&body);
            return st;
        }
        if (!entry_put(c, in_name, body.data, body.len) || !entry_marker(c, mk_name)) {
            buf_free(&body);
            run_error(run, "cannot write the initialization segment to the temporary file");
            return DASH_ERR_IO;
        }
        bytes += body.len;
        done++;
        buf_free(&body);
        tels_commit(c);
        report(run, bytes, done, total);
    }

    for (s = 0; s < nsrc; s++) {
        uint32_t i;
        for (i = 0; i < src[s].count; i++) {
            char sn[16], dn[16], url[DASH_MAX_URL], what[64];
            DashBuf body;
            DashStatus st;
            src_names(src[s].tag, i, sn, dn);
            if (entry_exists(c, dn)) continue;
            if (run_stop(run)) return DASH_ERR_STOPPED;
            if (!dash_rep_segment_url(src[s].rep, i, url, sizeof(url))) {
                run_error(run, "invalid segment address #%u", (unsigned)(i + 1u));
                return DASH_ERR_PARSE;
            }
            snprintf(what, sizeof(what), "%s segment #%u",
                     src[s].tag == 'v' ? "video" : "audio", (unsigned)(i + 1u));
            memset(&body, 0, sizeof(body));
            st = fetch_retry(run, url, DASH_SEG_MAX, &body, what);
            if (st != DASH_OK) {
                buf_free(&body);
                return st;
            }
            if (!entry_put(c, sn, body.data, body.len) || !entry_marker(c, dn)) {
                buf_free(&body);
                run_error(run, "cannot write segment #%u to the temporary file", (unsigned)(i + 1u));
                return DASH_ERR_IO;
            }
            bytes += body.len;
            done++;
            buf_free(&body);
            if (++uncommitted >= DASH_COMMIT_EVERY) {
                tels_commit(c);
                uncommitted = 0;
            }
            report(run, bytes, done, total);
        }
    }
    tels_commit(c);
    return DASH_OK;
}

/* ======================================================
   mux: گذر اول جمع‌آوری نمونه‌ها؛ گذر دوم نوشتن فایل
   ====================================================== */
typedef struct {
    int         src;           /* اندیس منبع */
    int         init_index;    /* اندیس track در init همان منبع */
    uint32_t    in_track_id;   /* track_ID در init */
    uint32_t    n, cap;
    uint32_t   *sizes;
    uint32_t   *durs;
    int32_t    *ctos;
    uint8_t    *sync;
    uint64_t    total_bytes;
    BOOL        has_cto;
    BOOL        has_nonsync;
} DashTrack;

typedef struct {
    DashSource *src;
    DashTrack  *tracks;
    int         map[2][MP4_MAX_TRACKS];   /* [منبع][اندیس init] → اندیس track */
    int         cur;
    BOOL        overflow;
} CollectCtx;

/* نوشتن بافرشده روی فایل خروجی */
typedef struct {
    HANDLE   h;
    uint8_t *buf;
    size_t   fill;
    size_t   cap;
    uint64_t written;
} OutWriter;

typedef struct {
    const uint8_t *seg;
    uint32_t       want_id;
    OutWriter     *w;
} CopyCtx;

static void track_free(DashTrack *t) {
    free(t->sizes);
    free(t->durs);
    free(t->ctos);
    free(t->sync);
    memset(t, 0, sizeof(*t));
}

static BOOL ow_flush(OutWriter *w) {
    DWORD n = 0;
    if (w->fill == 0) return TRUE;
    if (!WriteFile(w->h, w->buf, (DWORD)w->fill, &n, NULL) || n != (DWORD)w->fill) return FALSE;
    w->written += w->fill;
    w->fill = 0;
    return TRUE;
}

static BOOL ow_put(OutWriter *w, const uint8_t *p, size_t n) {
    while (n > 0) {
        size_t room = w->cap - w->fill;
        size_t take = n < room ? n : room;
        memcpy(w->buf + w->fill, p, take);
        w->fill += take;
        p += take;
        n -= take;
        if (w->fill == w->cap && !ow_flush(w)) return FALSE;
    }
    return TRUE;
}

static BOOL collect_cb(const Mp4FragSample *s, void *vctx) {
    CollectCtx *cc = (CollectCtx *)vctx;
    int ti = mp4_find_track(&cc->src[cc->cur].init, s->track_id);
    int k;
    DashTrack *t;
    if (ti < 0) return FALSE;
    k = cc->map[cc->cur][ti];
    if (k < 0) return FALSE;
    t = &cc->tracks[k];
    if (t->n >= DASH_MAX_SAMPLES) {
        cc->overflow = TRUE;
        return FALSE;
    }
    if (t->n == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2u : 4096u;
        uint32_t *a, *b;
        int32_t *o;
        uint8_t *y;
        if (nc > DASH_MAX_SAMPLES) nc = DASH_MAX_SAMPLES;
        a = (uint32_t *)realloc(t->sizes, (size_t)nc * sizeof(uint32_t));
        b = (uint32_t *)realloc(t->durs, (size_t)nc * sizeof(uint32_t));
        o = (int32_t *)realloc(t->ctos, (size_t)nc * sizeof(int32_t));
        y = (uint8_t *)realloc(t->sync, (size_t)nc);
        if (a) t->sizes = a;
        if (b) t->durs = b;
        if (o) t->ctos = o;
        if (y) t->sync = y;
        if (!a || !b || !o || !y) return FALSE;
        t->cap = nc;
    }
    t->sizes[t->n] = s->size;
    t->durs[t->n] = s->duration;
    t->ctos[t->n] = s->cto;
    t->sync[t->n] = s->sync ? 1u : 0u;
    if (s->cto != 0) t->has_cto = TRUE;
    if (!s->sync) t->has_nonsync = TRUE;
    t->total_bytes += s->size;
    t->n++;
    return TRUE;
}

static BOOL copy_cb(const Mp4FragSample *s, void *vctx) {
    CopyCtx *cp = (CopyCtx *)vctx;
    if (s->track_id != cp->want_id) return TRUE;
    return ow_put(cp->w, cp->seg + (size_t)s->offset, s->size);
}

static void free_sources(DashSource *src, int nsrc) {
    int s;
    for (s = 0; s < nsrc; s++) {
        buf_free(&src[s].init_buf);
        memset(&src[s].init, 0, sizeof(src[s].init));
    }
}

static DashStatus mux_output(DashRun *run, DashSource *src, int nsrc, TelsContainer *c,
                             const char *final_name) {
    DashTrack tracks[2 * MP4_MAX_TRACKS];
    CollectCtx cc;
    Mp4OutTrack ot[2 * MP4_MAX_TRACKS];
    DashTrack *used[2 * MP4_MAX_TRACKS];
    char err[256];
    int ntracks = 0, s, ti;
    uint32_t outn = 0, k, i;
    uint64_t payload = 0, ftyp_sz = 0, moov_sz = 0, total = 0;
    uint8_t mh[16];
    uint32_t mhl = 0;
    uint8_t ftyp[64];
    uint8_t *moov = NULL;
    BOOL use64;
    DashStatus res = DASH_OK;
    OutWriter w;
    HANDLE h = INVALID_HANDLE_VALUE;

    memset(tracks, 0, sizeof(tracks));
    memset(&cc, 0, sizeof(cc));
    memset(ot, 0, sizeof(ot));
    memset(&w, 0, sizeof(w));
    for (s = 0; s < 2; s++) {
        for (ti = 0; ti < MP4_MAX_TRACKS; ti++) cc.map[s][ti] = -1;
    }
    cc.src = src;
    cc.tracks = tracks;

    /* ۱. init ها و track های هر منبع */
    for (s = 0; s < nsrc && res == DASH_OK; s++) {
        char in_name[16], mk_name[16];
        const char *what = src[s].tag == 'v' ? "video" : "audio";
        init_names(src[s].tag, in_name, mk_name);
        if (!read_entry(c, in_name, &src[s].init_buf, DASH_INIT_MAX)) {
            run_error(run, "the temporary file lacks the %s initialization segment", what);
            res = DASH_ERR_IO;
            break;
        }
        if (!mp4_parse_init(src[s].init_buf.data, src[s].init_buf.len, &src[s].init,
                            err, sizeof(err))) {
            run_error(run, "the %s initialization segment is not valid MP4: %s", what, err);
            res = DASH_ERR_PARSE;
            break;
        }
        for (ti = 0; ti < src[s].init.track_count; ti++) {
            if (ntracks >= 2 * MP4_MAX_TRACKS) {
                run_error(run, "too many tracks");
                res = DASH_ERR_UNSUPPORTED;
                break;
            }
            cc.map[s][ti] = ntracks;
            tracks[ntracks].src = s;
            tracks[ntracks].init_index = ti;
            tracks[ntracks].in_track_id = src[s].init.track[ti].track_id;
            ntracks++;
        }
    }
    if (res != DASH_OK) goto cleanup;

    /* ۲. گذر اول: جمع‌آوری نمونه‌ها از همه‌ی segment ها */
    for (s = 0; s < nsrc && res == DASH_OK; s++) {
        cc.cur = s;
        for (i = 0; i < src[s].count && res == DASH_OK; i++) {
            char sn[16], dn[16];
            DashBuf seg;
            BOOL ok = TRUE;
            src_names(src[s].tag, i, sn, dn);
            memset(&seg, 0, sizeof(seg));
            if (!read_entry(c, sn, &seg, DASH_SEG_MAX)) {
                buf_free(&seg);
                run_error(run, "cannot read segment #%u from the temporary file", (unsigned)(i + 1u));
                res = DASH_ERR_IO;
                break;
            }
            if (seg.len > 0)
                ok = mp4_walk_fragments(seg.data, seg.len, &src[s].init, collect_cb, &cc,
                                        err, sizeof(err));
            buf_free(&seg);
            if (!ok) {
                if (cc.overflow)
                    run_error(run, "a track has more than %u samples", (unsigned)DASH_MAX_SAMPLES);
                else
                    run_error(run, "segment #%u is not valid MP4: %s", (unsigned)(i + 1u), err);
                res = DASH_ERR_PARSE;
                break;
            }
        }
    }
    if (res != DASH_OK) goto cleanup;

    /* ۳. فقط track های دارای نمونه؛ Mp4OutTrack برای هر کدام */
    for (k = 0; k < (uint32_t)ntracks; k++) {
        if (tracks[k].n > 0) used[outn++] = &tracks[k];
    }
    if (outn == 0) {
        run_error(run, "no samples were found in the downloaded segments");
        res = DASH_ERR_UNSUPPORTED;
        goto cleanup;
    }
    for (k = 0; k < outn; k++) {
        const DashTrack *t = used[k];
        const DashSource *sr = &src[t->src];
        const Mp4TrackInfo *inf = &sr->init.track[t->init_index];
        Mp4OutTrack *o = &ot[k];
        o->track_id = k + 1u;
        o->timescale = inf->timescale;
        memcpy(o->handler, inf->handler, sizeof(o->handler));
        o->width = inf->width;
        o->height = inf->height;
        o->language = inf->language;
        o->is_audio = (strcmp(inf->handler, "soun") == 0) ? TRUE : FALSE;
        o->stsd = inf->stsd;           o->stsd_len = inf->stsd_len;
        o->media_hdr = inf->media_hdr; o->media_hdr_len = inf->media_hdr_len;
        o->dinf = inf->dinf;           o->dinf_len = inf->dinf_len;
        o->hdlr = inf->hdlr;           o->hdlr_len = inf->hdlr_len;
        o->edts = inf->edts;           o->edts_len = inf->edts_len;
        o->src_movie_ts = sr->init.movie_timescale;
        o->sample_count = t->n;
        o->sizes = t->sizes;
        o->durations = t->durs;
        o->ctos = t->has_cto ? t->ctos : NULL;
        o->sync = t->has_nonsync ? t->sync : NULL;
        o->total_bytes = t->total_bytes;
        payload += t->total_bytes;
    }

    /* ۴. طرح فایل: ftyp و moov پیش از داده (برای پخش فوری) و سرآیند mdat */
    use64 = payload > 0x7FFFFFFFu ? TRUE : FALSE;
    ftyp_sz = mp4_write_ftyp(ftyp, sizeof(ftyp));
    moov_sz = mp4_write_moov(ot, (int)outn, 0, use64, NULL, 0);
    mhl = mp4_mdat_header(payload, mh);
    total = ftyp_sz + moov_sz + mhl + payload;
    moov = (uint8_t *)malloc((size_t)moov_sz);
    if (!moov) {
        run_error(run, "out of memory");
        res = DASH_ERR_IO;
        goto cleanup;
    }
    if (mp4_write_moov(ot, (int)outn, ftyp_sz + moov_sz + mhl, use64, moov, moov_sz) != moov_sz) {
        run_error(run, "internal error: moov size mismatch");
        res = DASH_ERR_IO;
        goto cleanup;
    }
    h = CreateFileA(final_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        run_error(run, "cannot create the output file (error %lu)", (unsigned long)GetLastError());
        res = DASH_ERR_IO;
        goto cleanup;
    }
    w.h = h;
    w.cap = DASH_OUT_BUF;
    w.buf = (uint8_t *)malloc(w.cap);
    if (!w.buf) {
        run_error(run, "out of memory");
        res = DASH_ERR_IO;
        goto finish_file;
    }
    if (!ow_put(&w, ftyp, (size_t)ftyp_sz) || !ow_put(&w, moov, (size_t)moov_sz) ||
        !ow_put(&w, mh, (size_t)mhl)) {
        run_error(run, "cannot write the output file");
        res = DASH_ERR_IO;
    }

    /* ۵. گذر دوم: داده‌ی هر track پشت سر هم، به ترتیب segment ها */
    for (k = 0; k < outn && res == DASH_OK; k++) {
        const DashTrack *t = used[k];
        DashSource *sr = &src[t->src];
        for (i = 0; i < sr->count && res == DASH_OK; i++) {
            char sn[16], dn[16];
            DashBuf seg;
            CopyCtx cp;
            src_names(sr->tag, i, sn, dn);
            if (run_stop(run)) {
                res = DASH_ERR_STOPPED;
                break;
            }
            memset(&seg, 0, sizeof(seg));
            if (!read_entry(c, sn, &seg, DASH_SEG_MAX)) {
                buf_free(&seg);
                run_error(run, "cannot read segment #%u from the temporary file", (unsigned)(i + 1u));
                res = DASH_ERR_IO;
                break;
            }
            if (seg.len > 0) {
                cp.seg = seg.data;
                cp.want_id = t->in_track_id;
                cp.w = &w;
                if (!mp4_walk_fragments(seg.data, seg.len, &sr->init, copy_cb, &cp, err, sizeof(err))) {
                    run_error(run, "cannot write the output file");
                    res = DASH_ERR_IO;
                }
            }
            buf_free(&seg);
        }
    }

finish_file:
    if (res == DASH_OK && !ow_flush(&w)) {
        run_error(run, "cannot write the output file");
        res = DASH_ERR_IO;
    }
    if (res == DASH_OK && w.written != total) {
        run_error(run, "internal error: output size mismatch");
        res = DASH_ERR_IO;
    }
    if (res == DASH_OK && !FlushFileBuffers(h)) {
        run_error(run, "cannot flush the output file");
        res = DASH_ERR_IO;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(w.buf);
    if (res != DASH_OK) DeleteFileA(final_name);

cleanup:
    free(moov);
    for (k = 0; k < (uint32_t)ntracks; k++) track_free(&tracks[k]);
    return res;
}

/* ======================================================
   یک دانلود کامل: انتخاب representation ها، کانتینر، ادامه یا شروع تازه، mux
   ====================================================== */
static DashStatus download_manifest(DashRun *run, const DASH_Manifest *mf) {
    DashSource src[2];
    int nsrc = 0, s;
    TelsContainer *c;
    uint64_t fp = DASH_FNV_BASIS, got_fp = 0;
    char final_name[IDM_MAX_PATH];
    DashStatus st;
    BOOL resume;

    memset(src, 0, sizeof(src));
    if (mf->video_count > 0 && mf->best_video < mf->video_count) {
        src[nsrc].rep = &mf->video[mf->best_video];
        src[nsrc].tag = 'v';
        nsrc++;
    }
    if (mf->audio_count > 0 && mf->best_audio < mf->audio_count) {
        src[nsrc].rep = &mf->audio[mf->best_audio];
        src[nsrc].tag = 'a';
        nsrc++;
    }
    if (nsrc == 0) {
        run_error(run, "the manifest has no supported video or audio representation");
        return DASH_ERR_UNSUPPORTED;
    }
    for (s = 0; s < nsrc; s++) {
        const char *what = src[s].tag == 'v' ? "video" : "audio";
        src[s].count = dash_rep_segment_count(src[s].rep);
        if (src[s].count == 0) {
            run_error(run, "cannot determine the number of %s segments", what);
            return DASH_ERR_UNSUPPORTED;
        }
        if (src[s].rep->init_url[0] == '\0') {
            run_error(run, "the %s representation has no initialization segment", what);
            return DASH_ERR_UNSUPPORTED;
        }
        if (!fingerprint_source(&src[s], &fp)) {
            run_error(run, "invalid %s segment address", what);
            return DASH_ERR_PARSE;
        }
    }

    c = open_container(run);
    if (!c) {
        if (run_stop(run)) return DASH_ERR_STOPPED;
        run_error(run, "temporary file is busy or damaged");
        return DASH_ERR_IO;
    }
    resume = meta_load(c, &got_fp, final_name, sizeof(final_name)) && got_fp == fp;
    if (!resume) {
        char text[512];
        int tl;
        if (!container_clear(c)) {
            finish_container(c);
            run_error(run, "cannot reset the temporary file");
            return DASH_ERR_IO;
        }
        choose_final_path(run->out_path, final_name, sizeof(final_name));
        tl = meta_format(text, sizeof(text), fp, final_name);
        if (tl <= 0 || !entry_put(c, DASH_META_NAME, (const uint8_t *)text, (uint64_t)tl)) {
            finish_container(c);
            run_error(run, "cannot write the temporary file");
            return DASH_ERR_IO;
        }
        tels_commit(c);
    }

    st = fetch_pending(run, src, nsrc, c);
    if (st == DASH_OK) st = mux_output(run, src, nsrc, c, final_name);
    free_sources(src, nsrc);
    if (st != DASH_OK) {
        finish_container(c);
        return st;
    }
    tels_close(c);
    if (!DeleteFileA(run->temp_path))
        LOGW(LOG_MOD_DASH, "temporary file %s could not be removed", run->temp_path);
    str_copy(run->final_path, sizeof(run->final_path), final_name);
    LOGI(LOG_MOD_DASH, "complete: %d source(s) muxed into %s", nsrc, final_name);
    return DASH_OK;
}

/* ======================================================
   نقطه‌ی ورود
   ====================================================== */
BOOL dash_download_wants(const char *url) {
    return dash_is_mpd_url(url);
}

DashStatus dash_download_run(DashRun *run) {
    DashBuf mpd;
    DASH_Manifest *mf;
    DashStatus st;
    if (!run || !run->url || !run->temp_path || !run->out_path) return DASH_ERR_IO;
    run->final_path[0] = '\0';
    run->error[0] = '\0';
    if (run_stop(run)) return DASH_ERR_STOPPED;

    mf = (DASH_Manifest *)calloc(1, sizeof(*mf));
    if (!mf) {
        run_error(run, "out of memory");
        return DASH_ERR_IO;
    }
    memset(&mpd, 0, sizeof(mpd));
    st = fetch_retry(run, run->url, DASH_MPD_MAX, &mpd, "manifest");
    if (st == DASH_OK) {
        if (!dash_parse_manifest(run->url, (const char *)mpd.data, (size_t)mpd.len, mf)) {
            run_error(run, "the URL is not a DASH manifest (no MPD element)");
            st = DASH_ERR_PARSE;
        } else if (mf->unsupported[0] != '\0') {
            run_error(run, "%s", mf->unsupported);
            st = DASH_ERR_UNSUPPORTED;
        } else {
            st = download_manifest(run, mf);
        }
    }
    buf_free(&mpd);
    dash_manifest_free(mf);
    free(mf);
    return st;
}
