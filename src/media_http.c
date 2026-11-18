/* media_http.c — پیاده‌سازی HTTP و بافر مشترک؛ رجوع کن به media_http.h */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_http.h"
#include "http.h"

#define MEDIA_READ_CHUNK     (64u * 1024u)
#define MEDIA_RETRY_CAP_SEC  60u
#define MEDIA_BUF_START      (1024u * 1024u)

/* نتیجه‌ی یک GET خام؛ جزئیات برای تصمیم تلاش مجدد */
typedef enum {
    ONCE_OK = 0,
    ONCE_NETWORK,        /* خطای شبکه یا HTTP؛ *status کد HTTP یا ۰ */
    ONCE_RANGE_IGNORED,  /* سرور Range را نادیده گرفت (200 به‌جای 206) */
    ONCE_TOO_LARGE,      /* بدنه از سقف بزرگ‌تر است یا حافظه کم است */
    ONCE_STOPPED
} OnceResult;

/* همان قاعدهٔ http.c (discovery) و engine.c: فقط این چهار کد دائمی‌اند.
   403 و 408 و 429 و 5xx موقتی‌اند، چون ممکن است محدودیت نرخ یا خطای گذرا باشند. */
static BOOL status_is_permanent(int st) {
    return st == 400 || st == 401 || st == 404 || st == 410;
}

void media_buf_free(MediaBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

BOOL media_buf_reserve(MediaBuf *b, uint64_t need, uint64_t max) {
    uint64_t cap;
    uint8_t *n;
    if (need <= b->cap) return TRUE;
    if (need > max) return FALSE;
    cap = b->cap ? b->cap : MEDIA_BUF_START;
    while (cap < need) cap *= 2u;
    if (cap > max) cap = max;
    n = (uint8_t *)realloc(b->data, (size_t)cap);
    if (!n) return FALSE;
    b->data = n;
    b->cap = cap;
    return TRUE;
}

BOOL media_stopped(const MediaHttp *h) {
    return (h->should_stop && h->should_stop(h->stop_ctx)) ? TRUE : FALSE;
}

BOOL media_sleep_checked(const MediaHttp *h, uint32_t ms) {
    uint32_t left = ms;
    while (left > 0) {
        uint32_t step;
        if (media_stopped(h)) return FALSE;
        step = left > 100u ? 100u : left;
        Sleep(step);
        left -= step;
    }
    return !media_stopped(h);
}

uint32_t media_retry_wait_ms(uint32_t attempt, uint32_t retry_after_sec) {
    uint32_t sec;
    if (retry_after_sec > 0u) {
        sec = retry_after_sec;
    } else if (attempt == 0u) {
        sec = 1u;
    } else if (attempt <= 6u) {
        sec = 1u << (attempt - 1u);
    } else {
        sec = MEDIA_RETRY_CAP_SEC;
    }
    if (sec > MEDIA_RETRY_CAP_SEC) sec = MEDIA_RETRY_CAP_SEC;
    return sec * 1000u;
}

/* یک GET؛ بدنه در out. کد جزئی: *status ۰ شبکه، -1 Range نادیده، -2 بزرگ‌تر از سقف */
static OnceResult http_once(const MediaHttp *h, const char *url, uint64_t start, uint64_t end,
                            uint64_t max_len, BOOL need_partial, MediaBuf *out,
                            int *status, uint32_t *retry_after) {
    uint32_t ra = 0;
    int st = 0;
    HTTP_Conn *conn;
    out->len = 0;
    *status = 0;
    *retry_after = 0;
    conn = http_open_range_status(url, h->referer, h->dl, start, end, &ra, &st);
    if (!conn) {
        *status = st;
        *retry_after = ra;
        return ONCE_NETWORK;
    }
    if (need_partial && !http_conn_got_partial(conn)) {
        http_close(conn);
        *status = -1;
        return ONCE_RANGE_IGNORED;
    }
    for (;;) {
        uint64_t room;
        uint32_t want;
        int n;
        if (media_stopped(h)) {
            http_close(conn);
            return ONCE_STOPPED;
        }
        if (out->len >= max_len) {
            /* سقف رسیده؛ اگر هنوز داده هست، بدنه بزرگ‌تر از حد است */
            uint8_t probe = 0;
            int pn = http_read(conn, &probe, 1);
            http_close(conn);
            if (pn > 0) {
                *status = -2;
                return ONCE_TOO_LARGE;
            }
            if (pn < 0) return ONCE_NETWORK;
            return ONCE_OK;
        }
        room = max_len - out->len;
        want = room > MEDIA_READ_CHUNK ? MEDIA_READ_CHUNK : (uint32_t)room;
        if (!media_buf_reserve(out, out->len + want, max_len)) {
            http_close(conn);
            *status = -2;
            return ONCE_TOO_LARGE;
        }
        n = http_read(conn, out->data + out->len, (int)want);
        if (n < 0) {
            http_close(conn);
            return ONCE_NETWORK;
        }
        if (n == 0) {
            http_close(conn);
            return ONCE_OK;
        }
        out->len += (uint64_t)n;
    }
}

MediaResult media_fetch(const MediaHttp *h, const char *url, uint64_t start, uint64_t end,
                        uint64_t max_len, BOOL need_partial, uint32_t attempts,
                        MediaBuf *out, const char *what, char *err, size_t err_sz) {
    int last = 0;
    uint32_t a;
    if (err && err_sz) err[0] = '\0';
    for (a = 1; a <= attempts; a++) {
        int st = 0;
        uint32_t ra = 0;
        OnceResult r = http_once(h, url, start, end, max_len, need_partial, out, &st, &ra);
        if (r == ONCE_OK) return MEDIA_OK;
        if (r == ONCE_STOPPED) return MEDIA_STOPPED;
        last = st;
        if (r == ONCE_RANGE_IGNORED) {
            snprintf(err, err_sz, "%s: the server ignored the byte range request", what);
            return MEDIA_FAILED;
        }
        if (r == ONCE_TOO_LARGE) {
            snprintf(err, err_sz, "%s is larger than the supported size", what);
            return MEDIA_FAILED;
        }
        if (status_is_permanent(st)) {
            snprintf(err, err_sz, "%s: HTTP %d", what, st);
            return MEDIA_NOT_FOUND;
        }
        if (a == attempts) break;
        if (!media_sleep_checked(h, media_retry_wait_ms(a, ra))) return MEDIA_STOPPED;
    }
    if (last > 0)
        snprintf(err, err_sz, "%s: HTTP %d after %u attempts", what, last, (unsigned)attempts);
    else
        snprintf(err, err_sz, "%s: network error after %u attempts", what, (unsigned)attempts);
    return MEDIA_FAILED;
}
