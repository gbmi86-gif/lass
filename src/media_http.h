/* media_http.h — HTTP و بافر مشترک دانلودهای HLS و DASH
   ─────────────────────────────────────────────────────────────
   یک GET (با Range اختیاری) و تلاش مجدد با یک قاعدهٔ واحد، همان قاعدهٔ
   http.c (discovery) و engine.c:
     - 400، 401، 404، 410: دائمی؛ بدون تلاش مجدد
     - بقیه‌ی خطاها، شامل 403، 408، 429 و 5xx: موقتی؛ تلاش مجدد
     - Retry-After سرور جای بک‌آف را می‌گیرد؛ هر انتظار حداکثر ۶۰ ثانیه
     - بدون Retry-After: ۱، ۲، ۴ ثانیه
   پیام خطا همیشه «نام: دلیل» است.
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_MEDIA_HTTP_H
#define LASS_MEDIA_HTTP_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"

struct IDM_Download;

/* بافر بدنه؛ هر دانلودر سقف خودش را در هر فراخوانی می‌دهد */
typedef struct MediaBuf {
    uint8_t *data;
    uint64_t len;
    uint64_t cap;
} MediaBuf;

/* آنچه HTTP لازم دارد؛ دانلودرها از روی run خودشان می‌سازند */
typedef struct MediaHttp {
    struct IDM_Download *dl;   /* هدر، کوکی و پراکسی؛ می‌تواند NULL باشد */
    const char          *referer;
    int                (*should_stop)(void *ctx);
    void               *stop_ctx;
} MediaHttp;

typedef enum {
    MEDIA_OK = 0,
    MEDIA_STOPPED,      /* توقف درخواستی */
    MEDIA_NOT_FOUND,    /* 400/401/404/410: خطای دائمی */
    MEDIA_FAILED        /* موقتی که بعد از تلاش‌ها رفع نشد، Range نادیده گرفته شد، یا بدنه بزرگ است */
} MediaResult;

void        media_buf_free(MediaBuf *b);
BOOL        media_buf_reserve(MediaBuf *b, uint64_t need, uint64_t max);
BOOL        media_stopped(const MediaHttp *h);
/* خواب کوتاه‌شده با بررسی توقف؛ FALSE یعنی متوقف شد */
BOOL        media_sleep_checked(const MediaHttp *h, uint32_t ms);
/* مدت انتظار پیش از تلاش بعدی؛ attempt از ۱ شروع می‌شود، retry_after_sec مقدار سرور یا ۰ */
uint32_t    media_retry_wait_ms(uint32_t attempt, uint32_t retry_after_sec);

/* یک GET کامل (start=end=0) یا زیربازه‌ی Range (need_partial: سرور باید 206 بدهد).
   out بدنه را می‌گیرد؛ در خطا err پیام قابل فهم می‌گیرد. */
MediaResult media_fetch(const MediaHttp *h, const char *url, uint64_t start, uint64_t end,
                        uint64_t max_len, BOOL need_partial, uint32_t attempts,
                        MediaBuf *out, const char *what, char *err, size_t err_sz);

#endif /* LASS_MEDIA_HTTP_H */
