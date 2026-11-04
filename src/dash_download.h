/* dash_download.h — دانلود کامل DASH با resume در سطح segment و mux به MP4
   ─────────────────────────────────────────────────────────────
   روی کانتینر موقت (.tels) کار می‌کند: init و هر segment صوتی/تصویری ذخیره و با
   نشانه‌ی جداگانه کامل بودنش ثبت می‌شود. اجرای بعدی فقط segment های ناکامل را
   می‌گیرد. وقتی همه کامل شد، قطعات به یک MP4 با moov جلو تبدیل می‌شوند.

   این مسیر جدا از دانلود HTTP معمولی است؛ موتور آن را در launch thread صدا می‌زند.
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_DASH_DOWNLOAD_H
#define LASS_DASH_DOWNLOAD_H

#include <windows.h>
#include <stdint.h>
#include "types.h"

struct IDM_Download;

typedef enum {
    DASH_OK = 0,
    DASH_ERR_FETCH,        /* خطای شبکه یا HTTP که بعد از تلاش‌های مجدد هم رفع نشد */
    DASH_ERR_NOT_FOUND,    /* 401/403/404/410: منبع وجود ندارد یا دسترسی ندارد */
    DASH_ERR_PARSE,        /* manifest یا segment معتبر نیست */
    DASH_ERR_UNSUPPORTED,  /* live، DRM، SegmentTimeline و موارد ناپشتیبانی‌شده */
    DASH_ERR_IO,           /* کانتینر یا فایل خروجی */
    DASH_ERR_STOPPED       /* توقف درخواستی (pause، cancel، خاموش شدن) */
} DashStatus;

typedef struct DashRun {
    struct IDM_Download *dl;        /* هدر، کوکی و پراکسی؛ می‌تواند NULL باشد */
    const char          *url;       /* آدرس MPD */
    const char          *referer;   /* اختیاری */
    const char          *temp_path; /* کانتینر .tels برای resume */
    const char          *out_path;  /* مسیر پیشنهادی؛ پسوند روی .mp4 تنظیم می‌شود */

    /* برگرداندن غیرصفر یعنی متوقف شو. نباید بلاک کند. */
    int                (*should_stop)(void *ctx);
    void               *stop_ctx;
    /* bytes_done: بایت‌های کامل؛ done/total: واحد‌های کامل (init و segment) */
    void               (*on_progress)(uint64_t bytes_done, uint32_t done,
                                      uint32_t total, void *ctx);
    void               *progress_ctx;

    char                final_path[IDM_MAX_PATH];  /* بعد از DASH_OK */
    char                error[160];                /* بعد از خطا: دلیل قابل‌فهم */
} DashRun;

/* آیا این URL باید با مسیر DASH دانلود شود؟ */
BOOL       dash_download_wants(const char *url);
/* دانلود یا ادامه‌ی دانلود. blocking است و در launch thread اجرا می‌شود. */
DashStatus dash_download_run(DashRun *run);

#endif /* LASS_DASH_DOWNLOAD_H */
