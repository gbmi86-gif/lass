/* hls_download.h — دانلود کامل جریان HLS با resume در سطح segment
   ─────────────────────────────────────────────────────────────
   روی کانتینر موقت (فایل .tels دانلود) کار می‌کند: هر segment داخل کانتینر
   ذخیره و با یک نشانه‌ی جداگانه کامل بودنش ثبت می‌شود. اگر دانلود متوقف یا
   قطع شود، اجرای بعدی فقط segment های ناکامل را دوباره می‌گیرد. وقتی همه‌ی
   segment ها کامل شدند، فایل نهایی (ts یا mp4) نوشته و کانتینر حذف می‌شود.

   این مسیر جدا از دانلود HTTP معمولی است؛ موتور آن را در launch thread
   صدا می‌زند (engine.c).
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_HLS_DOWNLOAD_H
#define LASS_HLS_DOWNLOAD_H

#include <windows.h>
#include <stdint.h>
#include "types.h"

struct IDM_Download;

typedef enum {
    HLS_OK = 0,
    HLS_ERR_FETCH,        /* خطای شبکه یا HTTP که بعد از تلاش‌های مجدد هم رفع نشد */
    HLS_ERR_NOT_FOUND,    /* 401/403/404/410: منبع وجود ندارد یا دسترسی ندارد */
    HLS_ERR_PARSE,        /* playlist معتبر HLS نیست */
    HLS_ERR_UNSUPPORTED,  /* live، DRM، صدای جدا و موارد ناپشتیبانی‌شده‌ی دیگر */
    HLS_ERR_CRYPTO,       /* رمزگشایی ناموفق (کلید یا داده‌ی خراب) */
    HLS_ERR_IO,           /* کانتینر یا فایل خروجی */
    HLS_ERR_STOPPED       /* توقف درخواستی (pause، cancel، خاموش شدن) */
} HlsStatus;

typedef struct HlsRun {
    struct IDM_Download *dl;        /* هدر، کوکی و پراکسی؛ می‌تواند NULL باشد */
    const char          *url;       /* آدرس playlist (master یا media) */
    const char          *referer;   /* اختیاری */
    const char          *temp_path; /* کانتینر .tels برای resume */
    const char          *out_path;  /* مسیر پیشنهادی؛ پسوند بر اساس محتوا تنظیم می‌شود */

    /* برگرداندن غیرصفر یعنی متوقف شو. نباید بلاک کند. */
    int                (*should_stop)(void *ctx);
    void               *stop_ctx;
    /* bytes_done: بایت‌های segment های کامل؛ done/total: تعداد segment ها */
    void               (*on_progress)(uint64_t bytes_done, uint32_t done,
                                      uint32_t total, void *ctx);
    void               *progress_ctx;

    char                final_path[IDM_MAX_PATH];  /* بعد از HLS_OK */
    char                error[160];                /* بعد از خطا: دلیل قابل‌فهم */
} HlsRun;

/* آیا این URL باید با مسیر HLS دانلود شود؟ */
BOOL      hls_download_wants(const char *url);
/* دانلود یا ادامه‌ی دانلود. blocking است و در launch thread اجرا می‌شود. */
HlsStatus hls_download_run(HlsRun *run);

#endif /* LASS_HLS_DOWNLOAD_H */
