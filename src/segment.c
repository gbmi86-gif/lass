/* segment.c  –  segment worker thread + lifecycle */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "transport.h"
#include "segment.h"
#include "http.h"
#include "storage.h"

/* تعریف شده در engine.c — محدودیت سرعت سراسری از پلاگین
   Speed Scheduler. حتی اگه plugin اصلاً load نشه یا غیرفعال
   باشه، این متغیر صفر می‌مونه و هیچ اثری نداره. */
extern volatile uint64_t g_global_speed_limit;
#include "engine.h"   /* for IDM_Download definition */
#include "lass_log.h"

/* علت شکست را نگه می‌داریم تا لاگ و علت توقف بگویند چرا؛ باید زیر seg->lock صدا زده شود */
static void seg_note_error(IDM_Segment *seg, const char *reason) {
    snprintf(seg->last_error, sizeof(seg->last_error), "%s", reason);
}

/* ── Sleep قابل‌قطع ──
   Sleep() معمولی اصلاً cancel_flag رو چک نمی‌کنه. اگه کاربر دقیقاً
   وسط یه backoff طولانی (retry_count بالا → تا ۳۲ ثانیه، طبق فرمول
   2000*2^(retry_count-1)) روی Pause بزنه، worker thread تا پایان
   کامل همون مدت اصلاً متوجه cancel_flag نمی‌شه — این یه علت کاملاً
   جدا (غیر از بلاک‌شدن روی WinINet) برای zombie موندن segment بود
   و دقیقاً همینه که در تست واقعی روی Segment #4 دیده شد. این تابع
   همون مدت رو صبر می‌کنه ولی هر ۱۰۰ میلی‌ثانیه cancel_flag رو چک
   می‌کنه و فوراً برمی‌گرده. */
static void cancellable_sleep(IDM_Segment *seg, DWORD ms) {
    const DWORD step = 100;
    while (ms > 0 && !seg->cancel_flag) {
        DWORD chunk = (ms < step) ? ms : step;
        Sleep(chunk);
        ms -= chunk;
    }
}

/* ======================================================
   Worker thread
   ====================================================== */
static DWORD WINAPI seg_worker(LPVOID param) {
    IDM_Segment  *seg = (IDM_Segment *)param;
    IDM_Download *dl  = seg->dl;

    EnterCriticalSection(&seg->lock);
    seg->state = SEG_CONNECTING;
    LeaveCriticalSection(&seg->lock);

retry:
    /* Respect speed limit: checked per-chunk in the read loop */
    DlStream *conn = dl_stream_open_range(dl, seg->current, seg->end);
    if (!conn) {
        /* Back-off and retry */
        EnterCriticalSection(&seg->lock);
        seg->retry_count++;
        if (seg->retry_count > SEG_MAX_RETRY || seg->cancel_flag) {
            seg_note_error(seg, "connection could not be opened (retries exhausted)");
            seg->state = SEG_FAILED;
            LeaveCriticalSection(&seg->lock);
            return 1;
        }
        DWORD delay = 2000u * (1u << (seg->retry_count - 1)); /* 2s, 4s, 8s... */
        if (delay > 60000) delay = 60000;
        seg->state     = SEG_RETRYING;
        seg->retry_until = GetTickCount() + delay;
        LeaveCriticalSection(&seg->lock);
        cancellable_sleep(seg, delay);
        if (seg->cancel_flag) {
            EnterCriticalSection(&seg->lock);
            seg->state = SEG_PAUSED;
            LeaveCriticalSection(&seg->lock);
            return 0;
        }
        goto retry;
    }

    /* ── حیاتی: اگه این segment باید از وسط فایل شروع بشه (start>0)
       ولی سرور Range رو واقعاً honor نکرده (200 به‌جای 206)،
       یعنی الان کل فایل از صفر در حال فرستادن هست. اگه به این
       داده اجازه بدیم در آفست seg->current نوشته بشه، فایل خراب
       می‌شه (این دقیقاً علت اصلی ZIPهای damaged بود). باید fail
       کنیم تا engine کل دانلود رو با یه segment تنها retry کنه. */
    if (seg->start > 0 && !dl_stream_got_partial(conn)) {
        dl_stream_close(conn);
        EnterCriticalSection(&seg->lock);
        seg_note_error(seg, "server ignored the range request");
        seg->state = SEG_FAILED;
        LeaveCriticalSection(&seg->lock);
        LOGI(LOG_MOD_SEGMENT, "[Segment #%u] Server ignored Range request (got 200, "
                "not 206) — refusing to write to avoid corruption.",
                seg->id);
        return 1;
    }

    EnterCriticalSection(&seg->lock);
    seg->state      = SEG_DOWNLOADING;
    seg->last_tick  = GetTickCount();
    seg->last_bytes = 0;
    seg->low_speed_since = 0; /* پنجره‌ی stall تازه برای این تلاش */
    LeaveCriticalSection(&seg->lock);

    uint8_t *buf = (uint8_t *)malloc(SEG_BUF_SIZE);
    if (!buf) {
        dl_stream_close(conn);
        EnterCriticalSection(&seg->lock);
        seg_note_error(seg, "out of memory");
        seg->state = SEG_FAILED;
        LeaveCriticalSection(&seg->lock);
        return 1;
    }

    while (!seg->cancel_flag) {
        /* Check if we're done with this segment range */
        if (seg->end > 0 && seg->current > seg->end) break;

        int n = dl_stream_read(conn, buf, SEG_BUF_SIZE);
        if (n == 0) break;          /* EOF */
        if (n < 0)  {               /* error – یا timeout بعد از Pause، یا شکست واقعی شبکه */
            dl_stream_close(conn);
            free(buf);
            if (seg->cancel_flag) {
                /* این خطا احتمالاً نتیجه‌ی همون receive-timeout کوتاهیه
                   که عمداً گذاشتیم تا thread سریع cancel_flag رو ببینه
                   (کاربر Pause زده) — نباید retry/reconnect کنیم،
                   وگرنه Pause عملاً بی‌اثر می‌شد. */
                EnterCriticalSection(&seg->lock);
                seg->state = SEG_PAUSED;
                LeaveCriticalSection(&seg->lock);
                return 0;
            }
            EnterCriticalSection(&seg->lock);
            seg->retry_count++;
            if (seg->retry_count > SEG_MAX_RETRY) {
                seg_note_error(seg, "connection dropped while reading");
                seg->state = SEG_FAILED;
                LeaveCriticalSection(&seg->lock);
                return 1;
            }
            seg->state = SEG_RETRYING;
            LeaveCriticalSection(&seg->lock);
            cancellable_sleep(seg, 3000);
            if (seg->cancel_flag) {
                EnterCriticalSection(&seg->lock);
                seg->state = SEG_PAUSED;
                LeaveCriticalSection(&seg->lock);
                return 0;
            }
            goto retry;
        }

        /* Write to file */
        if (fw_write(dl->writer, seg->current, buf, (size_t)n) != 0) {
            dl_stream_close(conn);
            free(buf);
            EnterCriticalSection(&seg->lock);
            seg_note_error(seg, "disk write failed");
            seg->state = SEG_FAILED;
            LeaveCriticalSection(&seg->lock);
            return 1;
        }

        /* Update counters */
        EnterCriticalSection(&seg->lock);
        seg->current += (uint64_t)n;
        seg->written += (uint64_t)n;
        seg->consec_failures = 0;
        seg->parked_logged = false;
        LeaveCriticalSection(&seg->lock);

        /* Update global downloaded counter (atomic add) */
        InterlockedAdd64((LONGLONG volatile *)&dl->downloaded, (LONGLONG)n);

        /* Speed tracking */
        seg_update_speed(seg, (uint64_t)n);

        /* Speed limit throttle — هم per-download و هم global چک میشه،
           هرکدوم سخت‌گیرانه‌تر بود اعمال میشه. global limit از
           پلاگین Speed Scheduler میاد (اختیاری، اگه فعال نباشه
           مقدارش صفر = بدون اثر). */
        uint64_t effective_limit = dl->speed_limit;
        uint64_t global_limit = g_global_speed_limit; /* خواندن atomic، بدون lock */
        if (global_limit > 0) {
            if (effective_limit == 0 || global_limit < effective_limit)
                effective_limit = global_limit;
        }

        if (effective_limit > 0) {
            /* Simple token bucket: calculate how long to sleep */
            EnterCriticalSection(&seg->lock);
            double spd = seg->speed;
            LeaveCriticalSection(&seg->lock);
            if (spd > (double)effective_limit) {
                DWORD sleep_ms = (DWORD)(((spd / effective_limit) - 1.0) * 200.0);
                if (sleep_ms > 2000) sleep_ms = 2000;
                cancellable_sleep(seg, sleep_ms);
            }
        }
    }

    free(buf);
    dl_stream_close(conn);

    EnterCriticalSection(&seg->lock);
    if (seg->cancel_flag) {
        seg->state = SEG_PAUSED;
    } else if (seg->end > 0 && seg->current <= seg->end) {
        /* EOF قبل از رسیدن به انتهای range — server زود connection
           رو بست. این یه شکست جزئی هست، retry می‌کنیم. */
        seg->retry_count++;
        if (seg->retry_count > SEG_MAX_RETRY) {
            seg_note_error(seg, "connection closed before the range ended");
            seg->state = SEG_FAILED;
        } else {
            seg->state = SEG_RETRYING;
            /* retry بعد از LeaveCriticalSection با goto */
        }
        LeaveCriticalSection(&seg->lock);
        if (seg->state == SEG_RETRYING) {
            cancellable_sleep(seg, 3000);
            if (seg->cancel_flag) {
                EnterCriticalSection(&seg->lock);
                seg->state = SEG_PAUSED;
                LeaveCriticalSection(&seg->lock);
                return 0;
            }
            goto retry;
        }
        return 1;
    } else {
        seg->state = SEG_COMPLETED;
    }
    LeaveCriticalSection(&seg->lock);

    return 0;
}

/* ======================================================
   Lifecycle
   ====================================================== */
IDM_Segment *seg_create(uint32_t id, uint64_t start, uint64_t end, IDM_Download *dl) {
    IDM_Segment *seg = (IDM_Segment *)calloc(1, sizeof(*seg));
    if (!seg) return NULL;

    seg->id      = id;
    seg->start   = start;
    seg->end     = end;
    seg->current = start;
    seg->written = 0;
    seg->state   = SEG_IDLE;
    seg->dl      = dl;
    seg->thread  = NULL;
    seg->cancel_flag = false;
    seg->zombie       = false;
    seg->lifecycle_busy = false;
    seg->low_speed_since = 0;
    InitializeCriticalSection(&seg->lock);
    return seg;
}

bool seg_destroy(IDM_Segment *seg) {
    if (!seg) return true;
    seg_pause(seg);
    if (seg->zombie) {
        /* worker thread هنوز واقعاً نمرده — آزاد کردن این حافظه الان
           یعنی use-after-free تضمین‌شده وقتی اون thread بالاخره
           برگرده و به seg دسترسی بزنه. ترجیح میدیم این ساختار
           کوچیک رو (به‌ندرت) leak کنیم تا crash کنیم. */
        LOGE(LOG_MOD_SEGMENT,
             "[Segment #%u] worker thread still alive — leaking segment "
             "struct instead of freeing it", seg->id);
        return false;
    }
    DeleteCriticalSection(&seg->lock);
    free(seg);
    return true;
}

void seg_start(IDM_Segment *seg) {
    if (!seg) return;
    seg->cancel_flag = false;
    seg->thread = CreateThread(NULL, 0, seg_worker, seg, 0, NULL);
}

void seg_pause(IDM_Segment *seg) {
    if (!seg) return;

    /* ── فیکس یه باگ ساختاری واقعی (علت crashهای مکرر) ──
       engine_pause عمداً seg_pause رو از UI thread و خارج از
       eng->lock صدا می‌زنه (تا کل موتور فریز نشه). dynamic_seg's
       monitor thread هم کاملاً مستقل، برای segmentهای stall-شده،
       seg_pause رو صدا می‌زنه. یعنی دو thread کاملاً غیرمرتبط
       می‌تونستن هم‌زمان seg_pause رو روی همون IDM_Segment اجرا
       کنن — هر دو seg->thread رو بخونن، هر دو CloseHandle بزنن
       (double-close یه HANDLE — اگه OS اون مقدار عددی رو در همون
       فاصله برای یه handle کاملاً بی‌ربط دیگه دوباره‌استفاده کرده
       باشه، این می‌تونه یه چیز کاملاً نامرتبط رو ببنده و باعث
       crash تصادفی در جای دیگه بشه). این دقیقاً با الگوی
       crashهای واقعی‌ای که دیدیم — بعد از چند تا stall/restart
       پشت‌سرهم، یه READ در آدرس صفر — کاملاً همخونی داره.

       فیکس: seg->lifecycle_busy این دو تابع رو نسبت به هم
       serialize می‌کنه. نمی‌تونیم seg->lock رو در کل مدت انتظار
       نگه داریم (چون worker thread خودش periodic به seg->lock
       نیاز داره؛ نگه داشتنش باعث deadlock می‌شد) — پس فقط برای
       چک/ست کردن همین یه flag ازش استفاده می‌کنیم و در فاصله‌ی
       انتظار با poll کوتاه (۵۰ms) دوباره چک می‌کنیم. */
    EnterCriticalSection(&seg->lock);
    while (seg->lifecycle_busy) {
        LeaveCriticalSection(&seg->lock);
        Sleep(50);
        EnterCriticalSection(&seg->lock);
    }
    if (seg->zombie) { LeaveCriticalSection(&seg->lock); return; }
    seg->lifecycle_busy = true;
    HANDLE h = seg->thread;
    LeaveCriticalSection(&seg->lock);

    seg->cancel_flag = true;

    /* ── اصلاح‌شده بعد از تست واقعی روی ویندوز ──
       نسخه‌ی قبلی این تابع سعی می‌کرد از همین‌جا (یه thread دیگه)
       با InternetCloseHandle روی conn->hRequest/hConnect، I/O
       بلاک‌شده‌ی worker thread رو abort کنه. لاگ واقعی نشون داد
       این کار به‌شکل قابل‌اعتماد کار نمی‌کنه (WinINet تضمین نمی‌کنه
       بستن handle از thread دیگه باعث برگشت فوری InternetReadFile
       بشه)، و چون هیچ lock مشترکی روی فیلدهای conn بین این thread
       و worker نبود، با بستن/آزادسازی همزمان همون conn توسط خود
       worker (وقتی طبیعی تموم می‌شد) race می‌کرد.

       راه‌حل درست و امن: دیگه از بیرون به conn دست نمی‌زنیم. در
       عوض timeout های WinINet (در http.c) کوتاه شدن (چند ثانیه
       به‌جای ۳۰-۶۰ ثانیه) تا خود worker thread، وقتی
       InternetReadFile به‌خاطر timeout برمی‌گرده، خودش cancel_flag
       رو ببینه و تمیز خارج بشه — بدون هیچ دست‌کاری cross-thread
       روی conn. */

    if (h) {
        /* باید حداقل به‌اندازه‌ی receive-timeout جدید (در http.c)
           بعلاوه‌ی یه margin صبر کنیم تا worker خودش با دیدن
           cancel_flag برگرده. */
        DWORD wr = WaitForSingleObject(h, 10000);
        EnterCriticalSection(&seg->lock);
        if (wr == WAIT_TIMEOUT) {
            /* واقعاً غیرمنتظره‌ست — برای ایمنی کامل، فرض نمی‌کنیم
               thread تموم شده. segment رو "زامبی" علامت می‌زنیم. */
            seg->zombie = true;
            seg->lifecycle_busy = false;
            LeaveCriticalSection(&seg->lock);
            LOGE(LOG_MOD_SEGMENT,
                 "[Segment #%u] worker thread did not stop within 10s "
                 "of Pause — marking zombie", seg->id);
            return;
        }
        /* فقط اگه هنوز همون handle ای هست که ما گرفتیم ببندیمش —
           جلوگیری از double-close اگه (به هر دلیل غیرمنتظره‌ای)
           یکی دیگه در همین فاصله عوضش کرده باشه. */
        if (seg->thread == h) {
            CloseHandle(h);
            seg->thread = NULL;
        }
        seg->lifecycle_busy = false;
        LeaveCriticalSection(&seg->lock);
    } else {
        EnterCriticalSection(&seg->lock);
        seg->lifecycle_busy = false;
        LeaveCriticalSection(&seg->lock);
    }
}

void seg_resume(IDM_Segment *seg) {
    if (!seg) return;

    EnterCriticalSection(&seg->lock);
    while (seg->lifecycle_busy) {
        LeaveCriticalSection(&seg->lock);
        Sleep(50);
        EnterCriticalSection(&seg->lock);
    }
    if (seg->zombie) {
        LeaveCriticalSection(&seg->lock);
        LOGE(LOG_MOD_SEGMENT,
             "[Segment #%u] refusing to resume a zombie segment", seg->id);
        return;
    }
    seg->lifecycle_busy = true;
    if (seg->thread) {
        CloseHandle(seg->thread);
        seg->thread = NULL;
    }
    seg->cancel_flag = false;
    seg->retry_count = 0;
    seg->total_resume_count++;
    seg->thread = CreateThread(NULL, 0, seg_worker, seg, 0, NULL);
    seg->lifecycle_busy = false;
    LeaveCriticalSection(&seg->lock);
}

void seg_wait(IDM_Segment *seg) {
    if (!seg || !seg->thread) return;
    WaitForSingleObject(seg->thread, INFINITE);
}

/* ======================================================
   Speed (EWMA)
   ====================================================== */
void seg_update_speed(IDM_Segment *seg, uint64_t new_bytes) {
    DWORD now   = GetTickCount();
    DWORD delta = now - seg->last_tick;
    if (delta == 0) delta = 1;

    double instant = (double)new_bytes / ((double)delta / 1000.0);
    /* EWMA alpha = 0.2 */
    if (seg->avg_speed == 0.0)
        seg->avg_speed = instant;
    else
        seg->avg_speed = 0.2 * instant + 0.8 * seg->avg_speed;

    seg->speed     = seg->avg_speed;
    seg->last_tick = now;
}

double seg_get_speed(IDM_Segment *seg) {
    if (!seg) return 0.0;
    EnterCriticalSection(&seg->lock);
    double s = seg->speed;
    LeaveCriticalSection(&seg->lock);
    return s;
}
