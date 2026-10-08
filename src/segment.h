/* segment.h  –  one download segment (byte range) */
#ifndef IDM_SEGMENT_H
#define IDM_SEGMENT_H

#include "types.h"

#define SEG_BUF_SIZE  (256 * 1024)   /* 256 KB read buffer per segment */
#define SEG_MAX_RETRY  5

/* ---- Segment structure ---- */
struct IDM_Segment {
    uint32_t      id;
    uint64_t      start;          /* byte range start */
    uint64_t      end;            /* byte range end (inclusive), 0 = unlimited */
    uint64_t      current;        /* next byte to write */
    uint64_t      written;        /* bytes written this session */
    SEG_State     state;

    /* Speed tracking */
    double        speed;          /* bytes/sec instant */
    double        avg_speed;      /* EWMA */
    DWORD         last_tick;      /* GetTickCount() */
    uint64_t      last_bytes;

    /* استفاده‌شده توسط dynamic_seg برای stall-detection مبتنی-بر-
       مدت (نه یه خوندن لحظه‌ای تنها). صفر یعنی الان سرعت پایین
       نیست؛ غیرصفر یعنی GetTickCount() لحظه‌ای که سرعت اولین‌بار
       زیر آستانه رفت — dynamic_seg این رو خودش ست/ریست می‌کنه.
       segment.c فقط دقیقاً وقتی یه اتصال تازه (start یا resume)
       وارد SEG_DOWNLOADING میشه صفرش می‌کنه، تا هر تلاش تازه یه
       پنجره‌ی stall کاملاً تازه داشته باشه — بدون این، یه segment
       تازه‌ساز که هنوز اولین بایتش نرسیده (avg_speed==0.0، کاملاً
       طبیعیه، نه stall) بلافاصله در همون tick بعدی مانیتور (تا ۲
       ثانیه بعد) به‌اشتباه «stall» تشخیص داده می‌شد و kill/restart
       می‌شد — قبل از اینکه اصلاً فرصت دریافت داده پیدا کنه. */
    DWORD         low_speed_since;

    /* Retry */
    uint32_t      retry_count;
    DWORD         retry_until;    /* GetTickCount() backoff deadline */

    /* تعداد کل دفعاتی که seg_resume صدا زده شده — برخلاف
       retry_count (که خودِ seg_resume صفرش می‌کنه)، این هیچ‌وقت
       صفر نمی‌شه؛ auto_retry_failed_segs توی engine.c از این
       استفاده می‌کنه تا واقعاً بعد از N تلاش دست بکشه، نه اینکه
       چون retry_count هر بار صفر می‌شه تا ابد retry کنه. */
    uint32_t      total_resume_count;

    /* Threading */
    HANDLE        thread;
    volatile bool cancel_flag;    /* set to stop worker */
    CRITICAL_SECTION lock;

    /* true یعنی worker thread حتی بعد از Pause (و بعد از صبر تا
       ۱۰ ثانیه) واقعاً تموم نشد (سناریوی خیلی نادر — باید فقط اگه
       WinINet timeout خودش کار نکنه رخ بده). در این حالت segment
       را آزاد/دوباره‌استفاده نمی‌کنیم — یه leak کوچیک، نه یه
       use-after-free. */
    volatile bool zombie;

    /* true یعنی یه thread دیگه همین الان داخل seg_pause/seg_resume
       این segment هست. چون engine_pause این‌ها رو از UI thread و
       خارج از eng->lock صدا می‌زنه (عمداً، تا فریز نشه)، و
       dynamic_seg's monitor thread هم کاملاً مستقل همین کار رو
       می‌کنه، دو thread می‌تونستن هم‌زمان seg_pause رو روی همون
       segment اجرا کنن — هر دو seg->thread رو بخونن، هر دو
       CloseHandle بزنن (double-close یه HANDLE، که می‌تونه یه
       handle کاملاً بی‌ربط دیگه رو ببنده اگه OS اون مقدار رو
       دوباره‌استفاده کرده باشه) — این دقیقاً همون use-after-free
       تصادفی (READ در آدرس صفر) بود که در تست‌های واقعی مکرر دیده
       شد. seg_pause/seg_resume الان با این فیلد سریالایز میشن. */
    volatile bool lifecycle_busy;

    /* Parent download (back-pointer) */
    IDM_Download *dl;
};

/* ---- Lifecycle ----
   seg_destroy نتیجه‌ی واقعی رو برمی‌گردونه: true یعنی segment واقعاً
   آزاد شد (memory دیگه معتبر نیست)، false یعنی worker thread هنوز
   زنده بود (zombie) و seg عمداً leak شد تا use-after-free رخ نده —
   caller در این حالت نباید به dl->writer/dl تکیه کنه که بی‌خطر
   بسته/آزاد میشه، چون zombie thread ممکنه هنوز بهش دسترسی داشته
   باشه. */
IDM_Segment *seg_create(uint32_t id, uint64_t start, uint64_t end, IDM_Download *dl);
bool         seg_destroy(IDM_Segment *seg);

/* ---- Control ---- */
void seg_start (IDM_Segment *seg);   /* spawn worker thread */
void seg_pause (IDM_Segment *seg);   /* signal cancel, wait thread */
void seg_resume(IDM_Segment *seg);   /* respawn thread from current offset */
void seg_wait  (IDM_Segment *seg);   /* WaitForSingleObject on thread */

/* ---- Speed ---- */
void   seg_update_speed(IDM_Segment *seg, uint64_t new_bytes);
double seg_get_speed   (IDM_Segment *seg);

#endif /* IDM_SEGMENT_H */
