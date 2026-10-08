/* engine.c  –  download scheduler & queue
   ─────────────────────────────────────────────────────────────
   رفع اشکال‌های نسخه قبل:

   ① freeze/Not Responding:
      http_discover (HEAD request شبکه) داخل scheduler_thread
      با eng->lock گرفته اجرا می‌شد → GUI برای گرفتن lock
      block می‌شد → Not Responding
      ✓ راه‌حل: DL_LAUNCHING state جدید + یه thread مجزا
        per download که discover رو خارج از lock انجام میده

   ② multi-download LIFO bug:
      queue_head = dl (stack) → دانلود جدیدتر همیشه جلو بود
      ✓ راه‌حل: FIFO با queue_tail — هر دانلود به انتها اضافه میشه

   ③ active_count race:
      start_download fail → active_count-- ولی قبلاً ++ نشده بود
      ✓ راه‌حل: active_count فقط در scheduler_thread تغییر می‌کنه،
        DL_LAUNCHING به عنوان "slot رزرو شده" حساب میشه

   ④ WinINet handle در http.c:
      g_inet singleton بدون sync → race در چند thread همزمان
      ✓ راه‌حل: هر launch_thread یه HINTERNET مستقل می‌گیره
        (از طریق http_discover_ex با پارامتر inet جداگانه)
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "engine.h"
#include "http.h"
#include "segment.h"
#include "storage.h"
#include "resume.h"
#include "lass_log.h"
#include "shutdown_brake.h"

/* ======================================================
   state اضافی برای راه‌اندازی async
   ====================================================== */
/* DL_LAUNCHING در types.h تعریف شده — نیاز به cast نیست */

/* ======================================================
   Internal helpers
   ====================================================== */

/* ======================================================
   حافظه‌ی «گفتگو» با هر host — نه دیکته‌ی همیشگی
   ======================================================
   قبلاً pick_seg_count فقط بر اساس حجم فایل تصمیم می‌گرفت، بدون
   هیچ حافظه‌ای از اینکه این host مشخص قبلاً چطور واکنش نشون داده.
   نتیجه: برای سرورهایی که واقعاً چند-اتصالی رو دوست ندارن (خیلی
   از mirror های دانلود مستقیم)، Lass هر بار از نو همون تعداد
   segment «زیاد» رو تحمیل می‌کرد، dynamic_seg هم مجبور می‌شد
   بارها stall واقعی تشخیص بده و segment ها رو بکشه/دوباره بسازه.

   این جدول کوچیک، برای هر host، تعداد stall واقعی اخیر رو نگه
   می‌داره (نه لحظه‌ای — فقط چیزی که seg_is_stalled واقعاً بعد از
   عبور از stall_timeout_sec تشخیص داده) و pick_seg_count از این
   برای شروع محتاطانه‌تر روی همون host استفاده می‌کنه. یه entry که
   مدتی (HOST_TROUBLE_DECAY_MS) مشکل جدید نداشته، فراموش میشه —
   یعنی سرور همیشه یه شانس تازه داره، این یه "قهر همیشگی" نیست. */
#define HOST_HISTORY_MAX         32
#define HOST_TROUBLE_MAX          6
#define HOST_TROUBLE_DECAY_MS (20u * 60u * 1000u) /* ۲۰ دقیقه */

typedef struct {
    char     host[128];
    uint32_t trouble;
    DWORD    last_trouble_tick;
    bool     used;
} HostHistoryEntry;

static HostHistoryEntry     g_host_history[HOST_HISTORY_MAX];
static CRITICAL_SECTION     g_host_history_lock;
static BOOL                 g_host_history_init = FALSE;

/* همون الگوی double-checked locking که در http.c برای g_inet_lock
   استفاده شده — برای یکدستی سبک کد. */
static void host_history_ensure_lock(void) {
    if (g_host_history_init) return;
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&g_host_history_lock);
        g_host_history_init = TRUE;
    } else {
        while (!g_host_history_init) Sleep(1);
    }
}

void engine_report_host_trouble(const char *host) {
    if (!host || !host[0]) return;
    host_history_ensure_lock();
    EnterCriticalSection(&g_host_history_lock);

    DWORD now = GetTickCount();
    int   slot = -1, lru_slot = 0;
    DWORD lru_tick = now;

    for (int i = 0; i < HOST_HISTORY_MAX; i++) {
        if (g_host_history[i].used && _stricmp(g_host_history[i].host, host) == 0) {
            slot = i;
            break;
        }
        if (!g_host_history[i].used && slot < 0) {
            slot = i; /* اولین جای خالی — ولی اگه بعداً match دقیق پیدا شد، اون اولویت داره */
        }
        if (g_host_history[i].used && g_host_history[i].last_trouble_tick < lru_tick) {
            lru_tick = g_host_history[i].last_trouble_tick;
            lru_slot = i;
        }
    }
    /* اگه match دقیق نبود و جای خالی هم نبود، قدیمی‌ترین رو evict کن */
    if (slot < 0) slot = lru_slot;

    HostHistoryEntry *e = &g_host_history[slot];
    if (!e->used || _stricmp(e->host, host) != 0) {
        snprintf(e->host, sizeof(e->host), "%s", host);
        e->trouble = 0;
        e->used = true;
    }
    if (e->trouble < HOST_TROUBLE_MAX) e->trouble++;
    e->last_trouble_tick = now;

    LeaveCriticalSection(&g_host_history_lock);
}

static uint32_t host_trouble_level(const char *host) {
    if (!host || !host[0]) return 0;
    host_history_ensure_lock();
    EnterCriticalSection(&g_host_history_lock);

    uint32_t level = 0;
    DWORD now = GetTickCount();
    for (int i = 0; i < HOST_HISTORY_MAX; i++) {
        if (g_host_history[i].used && _stricmp(g_host_history[i].host, host) == 0) {
            if (now - g_host_history[i].last_trouble_tick > HOST_TROUBLE_DECAY_MS) {
                /* مدتیه مشکل جدیدی نداشته — یه شانس تازه بده */
                g_host_history[i].used = false;
                g_host_history[i].trouble = 0;
            } else {
                level = g_host_history[i].trouble;
            }
            break;
        }
    }

    LeaveCriticalSection(&g_host_history_lock);
    return level;
}

static uint32_t pick_seg_count(const char *host, uint64_t size, uint32_t max_seg) {
    uint32_t n;
    if      (size == 0)                  n = 1;
    else if (size < 2ULL*1024*1024)      n = 1;
    else if (size < 10ULL*1024*1024)     n = 2;
    else if (size < 50ULL*1024*1024)     n = 4;
    else                                  n = max_seg;
    if (n > max_seg) n = max_seg;

    uint32_t trouble = host_trouble_level(host);
    if (trouble > 0 && n > 1) {
        /* ── گفتگو، نه دیکته ──
           این جدول بالا (size-based) همیشه همون تعداد segment رو
           برای یه host «اعلام» می‌کرد، بدون توجه به اینکه دفعات
           قبل این سرور مشخص چطور واکنش نشون داده — دقیقاً همون
           چیزی که Juliya بهش می‌گفت «دیکته کردن». الان اگه این
           host اخیراً چندین‌بار واقعاً stall/reject شده (رجوع کن
           به engine_report_host_trouble، که dynamic_seg موقع یه
           stall واقعی صداش می‌زنه)، شروع رو محتاطانه‌تر می‌کنیم —
           نه برای همیشه، فقط تا وقتی سرور دوباره ثابت کنه مشکلی
           نیست (رجوع کن به decay در host_trouble_level). اگه واقعاً
           سرعت خوب بود، try_split (در dynamic_seg) خودش دوباره
           segment اضافه می‌کنه — این خودِ تعریف یه گفتگوی واقعی:
           محتاطانه شروع کن، بر اساس واکنش واقعی سرور تطبیق بده. */
        if (n > trouble) n -= trouble; else n = 1;
    }
    return n;
}

uint32_t engine_test_pick_seg_count(const char *host, uint64_t size, uint32_t max_seg) {
    return pick_seg_count(host, size, max_seg);
}

/* ──────────────────────────────────────────────────────
   ① Launch thread: discover + start، خارج از eng->lock
   ────────────────────────────────────────────────────── */
typedef struct {
    IDM_Engine   *eng;
    IDM_Download *dl;
} LaunchCtx;

static DWORD WINAPI launch_thread_impl(LPVOID param) {
    LaunchCtx    *ctx = (LaunchCtx *)param;
    IDM_Engine   *eng = ctx->eng;
    IDM_Download *dl  = ctx->dl;
    free(ctx);

    /* ── ① discover (شبکه، بدون lock) ── */
    bool is_resume = false;
    {
        EnterCriticalSection(&eng->lock);
        is_resume = (dl->resume_segs != NULL);
        LeaveCriticalSection(&eng->lock);
    }

    if (!is_resume) {
        /* چک کن آیا engine در حال shutdown هست قبل از شروع HTTP */
        if (eng->shutdown) {
            EnterCriticalSection(&eng->lock);
            dl->state = DL_CANCELLED;
            eng->active_count--;
            LeaveCriticalSection(&eng->lock);
            return 0;
        }

        HTTP_DiscoveryInfo info;
        IDM_Result r = http_discover(dl->url, dl->referer, dl, &info);

        EnterCriticalSection(&eng->lock);

        /* اگه کاربر صراحتاً state رو عوض کرده (cancel/pause/remove)،
           این یه تصمیم آگاهانه‌ست — نباید retry کنیم، فوراً احترام
           بذاریم بهش. */
        if (dl->state != DL_LAUNCHING) {
            eng->active_count--;
            LeaveCriticalSection(&eng->lock);
            return 0;
        }

        if (r == IDM_ERR_NOT_FOUND) {
            /* خطای دائمی (404/410/400/401) — منبع واقعاً وجود
               نداره یا درخواست بدشکله. retry کردن هیچ‌وقت جواب
               نمیده، فقط چند دقیقه وقت کاربر و تلاش سرور رو تلف
               می‌کنه. فوراً fail کن، بدون backoff. */
            LOGE(LOG_MOD_ENGINE,
                 "dl#%u launch FAILED permanently (dead link — HTTP 404/410/400/401, "
                 "not retrying)", dl->id);
            strncpy(dl->fail_reason, "Link not found or invalid (404)",
                    sizeof(dl->fail_reason) - 1);
            dl->state = DL_FAILED;
            eng->active_count--;
            if (eng->notify_hwnd)
                PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                             (WPARAM)dl->id, (LPARAM)DL_FAILED);
            LeaveCriticalSection(&eng->lock);
            return 1;
        }

        if (r != IDM_OK) {
            /* شکست discovery — می‌تونه موقتی باشه (مثلاً سرور
               موقتاً connection رو رد کرده چون تعداد همزمان از
               این کلاینت زیاده). به‌جای DL_FAILED دائمی، چند بار
               با backoff افزایشی retry می‌کنیم — دقیقاً همون
               الگویی که برای segment ها در segment.c داریم. */
            dl->launch_retry_count++;
            if (dl->launch_retry_count <= ENGINE_MAX_LAUNCH_RETRY) {
                DWORD backoff_ms;
                bool  server_specified = (info.retry_after_sec > 0);
                if (server_specified) {
                    /* گفتگو، نه دیکته: سرور صراحتاً از طریق
                       Retry-After گفته چقدر صبر کنیم — به‌جای حدس
                       زدن خودمون با یه جدول exponential ثابت،
                       دقیقاً همون رو رعایت می‌کنیم (با یه سقف
                       امنیتی معقول). */
                    backoff_ms = info.retry_after_sec * 1000u;
                    if (backoff_ms > 300000u) backoff_ms = 300000u; /* سقف ۵ دقیقه */
                } else {
                    backoff_ms = 3000u * (1u << (dl->launch_retry_count - 1));
                    if (backoff_ms > 60000) backoff_ms = 60000; /* سقف ۶۰ ثانیه */
                }
                dl->next_launch_retry_at = GetTickCount() + backoff_ms;
                dl->state = DL_QUEUED; /* scheduler دوباره بعد از backoff امتحان می‌کنه */
                eng->active_count--;
                LOGW(LOG_MOD_ENGINE,
                     "dl#%u launch failed (attempt %u/%u) — retry in %lums%s",
                     dl->id, dl->launch_retry_count, ENGINE_MAX_LAUNCH_RETRY,
                     (unsigned long)backoff_ms,
                     server_specified ? " (server-requested via Retry-After)" : "");
                if (eng->notify_hwnd)
                    PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                                 (WPARAM)dl->id, (LPARAM)DL_QUEUED);
                LeaveCriticalSection(&eng->lock);
                return 1;
            }

            /* تلاش‌ها تموم شد — واقعاً fail کن */
            LOGE(LOG_MOD_ENGINE,
                 "dl#%u launch FAILED permanently after %u attempts",
                 dl->id, dl->launch_retry_count);
            strncpy(dl->fail_reason, "Connection failed after multiple retries",
                    sizeof(dl->fail_reason) - 1);
            dl->state = DL_FAILED;
            eng->active_count--;
            if (eng->notify_hwnd)
                PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                             (WPARAM)dl->id, (LPARAM)DL_FAILED);
            LeaveCriticalSection(&eng->lock);
            return 1;
        }

        /* موفقیت — شمارنده‌ی retry رو صفر کن */
        dl->launch_retry_count = 0;
        dl->next_launch_retry_at = 0;
        dl->fail_reason[0] = '\0';

        dl->total_size    = info.content_length;
        dl->accept_ranges = info.accept_ranges;
        snprintf(dl->etag, sizeof(dl->etag), "%s", info.etag);
        snprintf(dl->last_modified, sizeof(dl->last_modified), "%s",
                 info.last_modified);

        /* آپدیت نام فایل از Content-Disposition سرور — مخصوصاً
           برای CDN URL های توکن‌دار که اسم واقعی در هدر هست */
        if (info.filename[0] != '\0')
            memcpy(dl->filename, info.filename, IDM_MAX_FILENAME - 1);
        else if (dl->filename[0] == '\0')
            memcpy(dl->filename, info.filename, IDM_MAX_FILENAME - 1);

        /* اگه پسوند معتبر نداشت، از Content-Type اضافه کن */
        http_fixup_extension(dl->filename, IDM_MAX_FILENAME, info.content_type);

        /* حالا out_path و temp_path رو با نام واقعی بازسازی کن.
           مثلاً اگه قبلاً out_path = Downloads\9f8e7d6c.bin بود
           ولی server گفت filename=uabea-windows.zip، باید تغییر
           کنه به Downloads\uabea-windows.zip تا کاربر فایل درست
           با اسم درست ببینه. */
        if (dl->filename[0] != '\0') {
            /* لایه‌ی دفاعی دوم: مطمئن شو filename هیچ path
               separator ای نداره قبل از ترکیب با دایرکتوری.
               http_extract_filename از قبل sanitize می‌کنه، ولی
               این چک اضافه تضمین می‌کنه حتی اگه یه مسیر دیگه‌ی
               تغذیه‌ی filename در آینده اضافه بشه (مثلاً از یه
               پلاگین دیگه)، امنیت حفظ بشه — defense in depth. */
            for (char *p = dl->filename; *p; p++)
                if (*p == '\\' || *p == '/') *p = '_';

            /* پوشه رو از out_path قبلی استخراج کن */
            char dir[IDM_MAX_PATH] = {0};
            strncpy(dir, dl->out_path, IDM_MAX_PATH - 1);
            char *last_sep = NULL;
            for (char *p = dir; *p; p++)
                if (*p == '\\' || *p == '/') last_sep = p;
            if (last_sep) {
                *(last_sep + 1) = '\0'; /* dir = "C:\Downloads\" */
                size_t dir_len = strlen(dir);
                size_t fn_len  = strlen(dl->filename);
                if (dir_len + fn_len < (size_t)(IDM_MAX_PATH - 1)) {
                    char new_path[IDM_MAX_PATH] = {0};
                    memcpy(new_path, dir, dir_len);
                    memcpy(new_path + dir_len, dl->filename, fn_len);
                    new_path[dir_len + fn_len] = '\0';
                    strncpy(dl->out_path, new_path, IDM_MAX_PATH - 1);
                    int avail = IDM_MAX_PATH - 6;
                    snprintf(dl->temp_path, IDM_MAX_PATH, "%.*s.tels",
                             avail, dl->out_path);
                }
            }
        }
        LeaveCriticalSection(&eng->lock);
    }

    /* ── ② setup segments و file ── */
    EnterCriticalSection(&eng->lock);

    if (dl->state != DL_LAUNCHING) {
        /* کاربر در این فاصله cancel کرد */
        eng->active_count--;
        LeaveCriticalSection(&eng->lock);
        return 0;
    }

    bool ok = false;

    if (is_resume) {
        /* ── Resume ── */
        IDM_ResumeSeg *saved = dl->resume_segs;
        uint32_t nseg        = dl->num_segments;

        dl->segments = (IDM_Segment **)calloc(nseg, sizeof(IDM_Segment *));
        if (!dl->segments) goto fail;

        dl->writer = fw_open(dl->temp_path, dl->total_size, true);
        if (!dl->writer) goto fail;

        uint64_t already = 0;
        for (uint32_t i = 0; i < nseg; i++) {
            IDM_Segment *seg = seg_create(saved[i].id,
                                          saved[i].start,
                                          saved[i].end, dl);
            if (!seg) goto fail;
            seg->current = saved[i].current;
            seg->written = saved[i].current - saved[i].start;
            already += seg->written;
            dl->segments[i] = seg;
        }
        dl->downloaded = (LONGLONG)already;
        free(dl->resume_segs);
        dl->resume_segs = NULL;

        dl->state = DL_DOWNLOADING;
        for (uint32_t i = 0; i < nseg; i++) {
            if (dl->segments[i]->current < dl->segments[i]->end ||
                dl->segments[i]->end == 0)
                seg_start(dl->segments[i]);
            else
                dl->segments[i]->state = SEG_COMPLETED;
        }
        ok = true;

    } else {
        /* ── Fresh download ── */
        char host[128];
        http_extract_host(dl->url, host, sizeof(host));
        uint32_t nseg = (dl->accept_ranges && dl->total_size > 0)
                        ? pick_seg_count(host, dl->total_size, dl->max_segments)
                        : 1;
        dl->num_segments = nseg;
        dl->segments     = (IDM_Segment **)calloc(nseg, sizeof(IDM_Segment *));
        if (!dl->segments) goto fail;

        dl->writer = fw_open(dl->temp_path, dl->total_size, true);
        if (!dl->writer) goto fail;

        if (dl->total_size > 0) {
            uint64_t part = dl->total_size / nseg;
            for (uint32_t i = 0; i < nseg; i++) {
                uint64_t s = (uint64_t)i * part;
                uint64_t e = (i == nseg - 1)
                             ? (dl->total_size - 1)
                             : ((uint64_t)(i+1) * part - 1);
                dl->segments[i] = seg_create(i, s, e, dl);
                if (!dl->segments[i]) goto fail;
            }
        } else {
            dl->segments[0] = seg_create(0, 0, 0, dl);
            if (!dl->segments[0]) goto fail;
        }

        dl->state      = DL_DOWNLOADING;
        dl->downloaded = 0;

        for (uint32_t i = 0; i < nseg; i++)
            seg_start(dl->segments[i]);

        ok = true;
    }

fail:
    if (!ok) {
        dl->state = DL_FAILED;
        eng->active_count--;
        if (eng->notify_hwnd)
            PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                         (WPARAM)dl->id, (LPARAM)DL_FAILED);
        LeaveCriticalSection(&eng->lock);
        return 1;
    }

    if (eng->notify_hwnd)
        PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                     (WPARAM)dl->id, (LPARAM)DL_DOWNLOADING);

    LeaveCriticalSection(&eng->lock);
    return 0;
}

/* wrapper نازک که ثبت‌نام/لغو ثبت در ترمز خروج رو دور کل
   launch_thread_impl می‌پیچه — بدون دستکاری هیچ‌کدوم از ۵ نقطه‌ی
   return داخل تابع اصلی (که خودش پیچیدگی و منطق حساسی داره).
   این یعنی حتی اگه launch_thread_impl زودتر از موعد return کنه
   (fail شدن discovery، shutdown engine، و غیره)، unregister
   همیشه تضمین‌شده اجرا میشه. */
static DWORD WINAPI launch_thread(LPVOID param) {
    int brake_id = shutdown_brake_register("download_launch");
    DWORD result = launch_thread_impl(param);
    shutdown_brake_unregister(brake_id);
    return result;
}

/* ──────────────────────────────────────────────────────
   helper: شروع launch_thread برای یه دانلود
   ────────────────────────────────────────────────────── */
static void dispatch_launch(IDM_Engine *eng, IDM_Download *dl) {
    /* caller holds eng->lock */
    LaunchCtx *ctx = (LaunchCtx *)malloc(sizeof(*ctx));
    if (!ctx) {
        dl->state = DL_FAILED;
        if (eng->notify_hwnd)
            PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                         (WPARAM)dl->id, (LPARAM)DL_FAILED);
        return;
    }
    ctx->eng = eng;
    ctx->dl  = dl;
    dl->state = DL_LAUNCHING;
    eng->active_count++;

    HANDLE t = CreateThread(NULL, 0, launch_thread, ctx, 0, NULL);
    if (!t) {
        free(ctx);
        dl->state = DL_FAILED;
        eng->active_count--;
        return;
    }
    /* thread رو detach می‌کنیم — خودش تموم میشه */
    CloseHandle(t);
}

/* ======================================================
   Scheduler thread
   ====================================================== */
/* ── per-host stagger: چک و آپدیت ──
   caller باید eng->lock رو نگه داشته باشه. */
static bool host_stagger_should_wait(IDM_Engine *eng, const char *host) {
    if (!host || !host[0]) return false;
    DWORD now = GetTickCount();
    for (int i = 0; i < ENGINE_HOST_STAGGER_SLOTS; i++) {
        if (eng->host_stagger_name[i][0] &&
            _stricmp(eng->host_stagger_name[i], host) == 0) {
            DWORD elapsed = now - eng->host_stagger_at[i];
            return elapsed < ENGINE_HOST_STAGGER_MS;
        }
    }
    return false; /* این host هنوز در جدول نیست — نوبتش آزاده */
}

static void host_stagger_record(IDM_Engine *eng, const char *host) {
    if (!host || !host[0]) return;
    DWORD now = GetTickCount();

    /* اگه از قبل هست، فقط زمانش رو آپدیت کن */
    for (int i = 0; i < ENGINE_HOST_STAGGER_SLOTS; i++) {
        if (eng->host_stagger_name[i][0] &&
            _stricmp(eng->host_stagger_name[i], host) == 0) {
            eng->host_stagger_at[i] = now;
            return;
        }
    }
    /* جای خالی پیدا کن، یا قدیمی‌ترین رو جایگزین کن (LRU ساده) */
    int oldest_idx = 0;
    DWORD oldest_time = eng->host_stagger_at[0];
    for (int i = 0; i < ENGINE_HOST_STAGGER_SLOTS; i++) {
        if (!eng->host_stagger_name[i][0]) {
            strncpy(eng->host_stagger_name[i], host, 127);
            eng->host_stagger_at[i] = now;
            return;
        }
        if (eng->host_stagger_at[i] < oldest_time) {
            oldest_time = eng->host_stagger_at[i];
            oldest_idx  = i;
        }
    }
    strncpy(eng->host_stagger_name[oldest_idx], host, 127);
    eng->host_stagger_at[oldest_idx] = now;
}

static bool all_completed(IDM_Download *dl) {
    uint32_t done = 0;
    for (uint32_t i = 0; i < dl->num_segments; i++) {
        SEG_State ss = dl->segments[i]->state;
        if (ss == SEG_COMPLETED) done++;
        else if (ss == SEG_FAILED) return false;
    }
    return (done == dl->num_segments);
}

static bool any_failed(IDM_Download *dl) {
    for (uint32_t i = 0; i < dl->num_segments; i++)
        if (dl->segments[i]->state == SEG_FAILED) return true;
    return false;
}

static bool all_done_or_failed(IDM_Download *dl) {
    for (uint32_t i = 0; i < dl->num_segments; i++) {
        SEG_State s = dl->segments[i]->state;
        if (s == SEG_DOWNLOADING || s == SEG_CONNECTING || s == SEG_RETRYING)
            return false;
    }
    return true;
}

static void auto_retry_failed_segs(IDM_Download *dl, uint32_t max_retries) {
    for (uint32_t i = 0; i < dl->num_segments; i++) {
        IDM_Segment *seg = dl->segments[i];
        if (!seg) continue;
        EnterCriticalSection(&seg->lock);
        /* از total_resume_count استفاده می‌کنیم، نه retry_count —
           چون خودِ seg_resume هر بار retry_count رو صفر می‌کنه؛ اگه
           از همون استفاده می‌کردیم این تابع هیچ‌وقت واقعاً به سقف
           نمی‌رسید و یه لینک واقعاً خراب رو تا ابد retry می‌کرد
           (دقیقاً چیزی که در لاگ واقعی دیده شد: "attempt 7/10"
           بی‌نهایت تکرار می‌شد، هیچ‌وقت به ۱۰ نمی‌رسید). */
        bool should_retry = (seg->state == SEG_FAILED) &&
                            (seg->total_resume_count < max_retries);
        LeaveCriticalSection(&seg->lock);
        if (should_retry) {
            LOGI(LOG_MOD_ENGINE, "Auto-retry seg#%u (attempt %u/%u)",
                    seg->id, seg->total_resume_count + 1, max_retries);
            seg_resume(seg);
        }
    }
}

static DWORD WINAPI scheduler_thread(LPVOID param) {
    IDM_Engine *eng = (IDM_Engine *)param;

    /* ثبت‌نام در ترمز خروج — این scheduler اولین چیزیه که با
       engine_create شروع میشه و باید آخرین چیزی باشه که واقعاً
       خارج میشه، چون همه‌ی launch_thread ها و seg_worker ها به
       این وابسته‌ن (اگه اونها هنوز کار می‌کنن، این نباید زودتر
       از موعد بره). */
    int brake_id = shutdown_brake_register("engine_scheduler");

    while (!eng->shutdown) {
        WaitForSingleObject(eng->wake_event, 500);
        ResetEvent(eng->wake_event);
        if (eng->shutdown) break;

        EnterCriticalSection(&eng->lock);

        for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) {
            switch (dl->state) {

            /* ── ② FIFO: scheduler همه رو به ترتیب می‌بینه ── */
            case DL_QUEUED:
                /* اگه این دانلود در حال backoff بعد از یه شکست
                   قبلیه، صبر کن تا زمانش برسه — وگرنه scheduler
                   (که هر ۵۰۰ms اجرا میشه) فوراً دوباره تلاش
                   می‌کنه و کل idea پشت exponential backoff رو
                   بی‌اثر می‌کنه. */
                if (dl->next_launch_retry_at != 0 &&
                    GetTickCount() < dl->next_launch_retry_at) {
                    break;
                }
                if (eng->active_count < eng->max_concurrent) {
                    /* ── per-host stagger ──
                       اگه یه دانلود دیگه اخیراً (کمتر از
                       ENGINE_HOST_STAGGER_MS پیش) discovery‌ش رو
                       به همین host شروع کرده، صبر کن. این فقط
                       مرحله‌ی discovery (HEAD request) رو stagger
                       می‌کنه — بعد از launch موفق، segment های
                       همون دانلود آزادانه و همزمان کار می‌کنن،
                       چون این محدودیت فقط جلوی «چند دانلود مختلف
                       که همزمان شروع میشن» رو می‌گیره، نه
                       همزمانی طبیعی segment های یه دانلود واحد. */
                    char host[128];
                    http_extract_host(dl->url, host, sizeof(host));
                    if (host_stagger_should_wait(eng, host)) {
                        break; /* این tick صبر کن، دفعه‌ی بعد امتحان کن */
                    }
                    host_stagger_record(eng, host);

                    dispatch_launch(eng, dl);
                    if (eng->notify_hwnd)
                        PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                                     (WPARAM)dl->id, (LPARAM)dl->state);
                }
                break;

            case DL_PAUSED:
                /* ── زمان‌بند: بررسی رسیدن وقت ── */
                if (dl->has_schedule) {
                    FILETIME now_ft;
                    GetSystemTimeAsFileTime(&now_ft);
                    ULARGE_INTEGER t_now, t_sched;
                    t_now.LowPart    = now_ft.dwLowDateTime;
                    t_now.HighPart   = now_ft.dwHighDateTime;
                    t_sched.LowPart  = dl->scheduled_at.dwLowDateTime;
                    t_sched.HighPart = dl->scheduled_at.dwHighDateTime;
                    if (t_now.QuadPart >= t_sched.QuadPart) {
                        dl->has_schedule = FALSE;
                        memset(&dl->scheduled_at, 0, sizeof(dl->scheduled_at));
                        dl->state = DL_QUEUED;
                        LOGI(LOG_MOD_ENGINE, "Scheduled #%u triggered", dl->id);
                        if (eng->notify_hwnd)
                            PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                                         (WPARAM)dl->id, (LPARAM)DL_QUEUED);
                    }
                }
                break;

            case DL_LAUNCHING:
                /* launch_thread در حال کار، فقط progress بده */
                if (eng->notify_hwnd)
                    PostMessageA(eng->notify_hwnd, IDM_WM_PROGRESS,
                                 (WPARAM)dl->id, 0);
                break;

            case DL_DOWNLOADING:
                if (eng->notify_hwnd)
                    PostMessageA(eng->notify_hwnd, IDM_WM_PROGRESS,
                                 (WPARAM)dl->id, 0);

                if (any_failed(dl))
                    auto_retry_failed_segs(dl, 10);

                if (all_done_or_failed(dl)) {
                    if (all_completed(dl)) {
                        dl->state = DL_COMPLETED;
                        eng->active_count--;
                        resume_remove(dl);
                        fw_sync(dl->writer);
                        fw_close(dl->writer);
                        dl->writer = NULL;

                        /* فایل کامل شده — حالا از .tels به نام
                           نهایی واقعی rename می‌کنیم. این عملیات
                           atomic و سریعه (همون درایو، فقط تغییر
                           entry در فایل‌سیستم، نه کپی واقعی
                           داده‌ها). MOVEFILE_REPLACE_EXISTING برای
                           جایی که از قبل با همین اسم فایلی هست
                           (مثلاً resume بعد از یه تلاش قبلی). */
                        if (!MoveFileExA(dl->temp_path, dl->out_path,
                                        MOVEFILE_REPLACE_EXISTING)) {
                            LOGW(LOG_MOD_ENGINE, "rename .tels->final failed (err=%lu) remains as %s",
                                    (unsigned long)GetLastError(), dl->temp_path);
                        }

                        if (eng->notify_hwnd)
                            PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                                         (WPARAM)dl->id, (LPARAM)DL_COMPLETED);
                    } else if (any_failed(dl)) {
                        dl->state = DL_FAILED;
                        eng->active_count--;
                        resume_save(dl);
                        if (eng->notify_hwnd)
                            PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                                         (WPARAM)dl->id, (LPARAM)DL_FAILED);
                    }
                }
                break;

            default:
                /* DL_PAUSED, DL_COMPLETED, DL_CANCELLED, DL_LAUNCHING و بقیه */
                break;
            }
        }

        LeaveCriticalSection(&eng->lock);
    }
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ======================================================
   Public API
   ====================================================== */
IDM_Engine *engine_create(HWND notify_hwnd) {
    IDM_Engine *eng = (IDM_Engine *)calloc(1, sizeof(*eng));
    if (!eng) return NULL;

    eng->max_concurrent = 5;
    eng->notify_hwnd    = notify_hwnd;
    eng->shutdown       = false;
    eng->queue_head     = NULL;
    eng->queue_tail     = NULL;
    InitializeCriticalSection(&eng->lock);
    eng->wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    eng->scheduler  = CreateThread(NULL, 0, scheduler_thread, eng, 0, NULL);
    return eng;
}

void engine_signal_shutdown(IDM_Engine *eng) {
    if (!eng) return;
    eng->shutdown = true;
    SetEvent(eng->wake_event);
}

void engine_destroy(IDM_Engine *eng) {
    if (!eng) return;

    engine_signal_shutdown(eng);
    WaitForSingleObject(eng->scheduler, 5000);
    CloseHandle(eng->scheduler);
    CloseHandle(eng->wake_event);

    /* صبر برای تمام launch_thread های در حال اجرا — این thread ها
       از eng و dl دسترسی دارن. */
    DWORD wait_start = GetTickCount();
    while (eng->active_count > 0 &&
           (GetTickCount() - wait_start) < 5000) {
        Sleep(50);
    }

    if (eng->active_count > 0) {
        /* یه یا چند launch_thread هنوز زنده‌ن و مستقیم به eng/dl
           (و eng->lock) دسترسی دارن. آزاد کردن eng، هر dl، یا حتی
           خود eng->lock الان یعنی use-after-free تضمین‌شده — این
           دقیقاً همون crash‌ای بود که در تست دیدیم (launch thread
           برای "x.com/f.zip" بعد از timeout جواب گرفت، درحالی‌که
           engine_destroy قبلاً بدون توجه به این warning، eng/dl رو
           آزاد کرده بود). کل eng (به‌همراه صف دانلودهاش) رو عمداً
           leak می‌کنیم؛ در برنامه‌ی واقعی این فقط موقع بستن برنامه
           اتفاق می‌افته که OS به‌هرحال کل حافظه رو پس می‌گیره. */
        LOGE(LOG_MOD_ENGINE,
             "engine_destroy: %d launch thread(s) still active after "
             "5s — leaking entire engine (queue+downloads) instead of "
             "freeing, to avoid use-after-free", eng->active_count);
        return;
    }

    EnterCriticalSection(&eng->lock);
    IDM_Download *dl = eng->queue_head;
    while (dl) {
        IDM_Download *next = dl->next;
        bool any_zombie = false;
        for (uint32_t i = 0; i < dl->num_segments; i++)
            if (dl->segments && dl->segments[i])
                if (!seg_destroy(dl->segments[i])) any_zombie = true;
        free(dl->segments);
        if (any_zombie) {
            /* برنامه در حال بسته شدنه ولی یه segment thread هنوز
               زنده‌ست. dl رو نمی‌بندیم/آزاد نمی‌کنیم — یه leak که با
               خروج کامل process به‌هرحال توسط OS پاک میشه، خیلی
               بهتر از crash روی exit. */
            LOGE(LOG_MOD_ENGINE,
                 "dl#%u has a zombie segment during shutdown — leaking",
                 dl->id);
        } else {
            if (dl->writer) fw_close(dl->writer);
            free(dl->resume_segs);
            DeleteCriticalSection(&dl->lock);
            free(dl);
        }
        dl = next;
    }
    LeaveCriticalSection(&eng->lock);
    DeleteCriticalSection(&eng->lock);
    free(eng);
}

/* ── ② FIFO: به انتهای queue اضافه کن ── */
IDM_Download *engine_add(IDM_Engine  *eng,
                          const char  *url,
                          const char  *out_path,
                          uint32_t     max_segments,
                          uint64_t     speed_limit) {
    if (!eng || !url) return NULL;

    IDM_Download *dl = (IDM_Download *)calloc(1, sizeof(*dl));
    if (!dl) return NULL;

    dl->eng = eng;
    InitializeCriticalSection(&dl->lock);

    EnterCriticalSection(&eng->lock);
    dl->id           = eng->next_id++;
    dl->state        = DL_QUEUED;
    dl->max_segments = max_segments ? max_segments : 8;
    dl->speed_limit  = speed_limit;
    dl->next         = NULL;
    snprintf(dl->url,      IDM_MAX_URL,  "%s", url);
    /* فیکس دفاعی: قبلاً فقط NULL رو به "download.bin" fallback
       می‌داد؛ یه رشته‌ی *خالی* (که هر caller ای از جمله مسیرهایی که
       PathCombineA روشون شکست خورده بود می‌تونست بفرسته) به‌عنوان
       یه مسیر معتبر قبول می‌شد و بی‌صدا تا انتهای pipeline (temp_path
       = ".tels"، resume state، ...) می‌رفت. حالا هر دو یکسان
       fallback می‌شن. */
    snprintf(dl->out_path, IDM_MAX_PATH, "%s",
             (out_path && out_path[0]) ? out_path : "download.bin");

    /* فایل .tels — این همون چیزیه که واقعاً روی دیسک حین دانلود
       نوشته میشه. چرا پسوند مجزا؟
       ۱. اگه برنامه کرش کنه یا با Task Manager بسته بشه، فایل
          .tels (همراه با state file که resume.c می‌سازه) کامل
          روی دیسک باقی می‌مونه — هیچ داده‌ای گم نمیشه.
       ۲. هیچ‌وقت کاربر یه فایل نیمه‌کاره با اسم نهایی (مثلاً
          movie.mp4) نمی‌بینه که با دوبار-کلیک باز کنه و فکر کنه
          خرابه — تا وقتی واقعاً کامل نشده، اسمش .tels می‌مونه.
       ۳. بعد از اتمام موفق، یه rename ساده (نه کپی!) به نام
          نهایی انجام میشه — سریع و atomic در همون درایو. */
    /* محدود کردن out_path هنگام ساخت temp_path تا جای کافی برای
       ".tels" (۵ کاراکتر) + null باقی بمونه. بدون این، اگه
       out_path نزدیک به IDM_MAX_PATH باشه، snprintf می‌تونست
       truncate کنه — که هم warning کامپایلر می‌داد و هم در تئوری
       می‌تونست باعث تداخل نام بین دو دانلود مختلف با مسیر طولانی
       مشابه بشه. */
    int avail = IDM_MAX_PATH - 6; /* 5 برای ".tels" + 1 برای NUL */
    if (avail < 1) avail = 1;
    snprintf(dl->temp_path, IDM_MAX_PATH, "%.*s.tels",
             avail, dl->out_path);

    http_extract_filename(url, NULL, dl->filename, IDM_MAX_FILENAME);
    resume_load(dl);

    /* ── FIFO: به انتها وصل کن ── */
    if (eng->queue_tail) {
        eng->queue_tail->next = dl;
    } else {
        eng->queue_head = dl;
    }
    eng->queue_tail = dl;

    LeaveCriticalSection(&eng->lock);

    SetEvent(eng->wake_event);

    if (eng->notify_hwnd)
        PostMessageA(eng->notify_hwnd, IDM_WM_ADDED, (WPARAM)dl->id, 0);

    return dl;
}

void engine_set_referer(IDM_Download *dl, const char *referer) {
    if (!dl || !referer) return;
    EnterCriticalSection(&dl->lock);
    snprintf(dl->referer, IDM_MAX_URL, "%s", referer);
    /* جلوگیری از HTTP header injection: هیچ کاراکتر CR/LF نباید
       داخل مقدار یه header دیگه (مثل Referer) تزریق بشه، وگرنه
       یکی می‌تونست با یه referer دستکاری‌شده header دلخواه اضافه
       کنه. هر CR/LF رو با space جایگزین می‌کنیم. */
    for (char *p = dl->referer; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
    LeaveCriticalSection(&dl->lock);
}

/* ── محدودیت سرعت سراسری (Speed Scheduler plugin) ──
   یه متغیر global سطح process به‌جای فیلد داخل IDM_Engine —
   این انتخاب عمدی برای کاهش ریسک هست: تغییر دادن سایز/layout
   struct IDM_Engine می‌تونست هرجایی که از sizeof(IDM_Engine) یا
   memcpy/calloc روی این struct استفاده می‌کنه رو تحت تأثیر قرار
   بده. یه global ساده، مستقل، و کاملاً ایزوله از engine داخلیه.
   بدون lock امنه چون فقط یه عدد ساده هست (نه pointer)، و حتی
   race جزئی در بدترین حالت یه فریم throttle نادرست میده، نه
   crash یا corruption. */
volatile uint64_t g_global_speed_limit = 0;

void engine_set_global_speed_limit(IDM_Engine *eng, uint64_t bytes_per_sec) {
    (void)eng; /* پارامتر برای سازگاری API و امکان گسترش آینده نگه داشته شده */
    InterlockedExchange64((LONGLONG *)&g_global_speed_limit,
                          (LONGLONG)bytes_per_sec);
}

uint64_t engine_get_global_speed_limit(IDM_Engine *eng) {
    (void)eng;
    return (uint64_t)InterlockedCompareExchange64(
        (LONGLONG *)&g_global_speed_limit, 0, 0);
}

void engine_pause(IDM_Engine *eng, uint32_t id) {
    if (!eng) return;

    /* مرحله ۱: زیر lock فقط state رو تغییر بده و یه snapshot از
       segment pointer ها بگیر — seg_pause نباید زیر lock صدا
       زده بشه چون می‌تونه تا 10 ثانیه برای هر segment بلاک بشه
       (WaitForSingleObject). اگه این کار رو زیر eng->lock انجام
       بدیم، کل engine برای همه‌ی thread های دیگه (UI polling،
       سایر دانلودها) فریز می‌شه — این دقیقاً علت هنگ کردن واقعی
       دکمه‌ی Pause/Stop بود. */
    IDM_Segment **segs_snapshot = NULL;
    uint32_t      seg_count     = 0;
    bool          was_active    = false;
    IDM_Download *dl_ref        = NULL;

    EnterCriticalSection(&eng->lock);
    IDM_Download *dl = engine_find(eng, id);
    if (dl && (dl->state == DL_DOWNLOADING || dl->state == DL_LAUNCHING)) {
        was_active = true;
        /* عمداً DL_PAUSED نه — یه حالت گذرای DL_PAUSING تا وقتی
           segment thread ها واقعاً متوقف بشن. اگه اینجا مستقیم
           DL_PAUSED ست می‌شد، engine_resume می‌تونست همون لحظه
           (چون precondition‌ش state==PAUSED هست) شروع کنه به
           NULL کردن dl->segments، درحالی‌که همین تابع هنوز پایین‌تر
           داره از همون آرایه (resume_save) می‌خونه — دقیقاً همون
           race ای که باعث crash واقعی (null-deref) شده بود. */
        dl->state  = DL_PAUSING;
        dl_ref     = dl;
        seg_count  = dl->num_segments;
        if (seg_count > 0 && dl->segments) {
            segs_snapshot = (IDM_Segment **)malloc(
                sizeof(IDM_Segment *) * seg_count);
            if (segs_snapshot) {
                for (uint32_t i = 0; i < seg_count; i++)
                    segs_snapshot[i] = dl->segments[i];
            } else {
                seg_count = 0; /* malloc fail — رد می‌کنیم، بهتر از crash */
            }
        }
        if (was_active) eng->active_count--;
        /* توجه: اینجا هنوز notify نمی‌کنیم — پایین‌تر، وقتی واقعاً
           DL_PAUSED شد، پیام میره. اگه همین‌جا DL_PAUSED رو به UI/
           پلاگین‌ها اطلاع بدیم (که قبلاً می‌داد)، smart_resume
           پلاگین هم می‌تونست دقیقاً همون لحظه sr_save رو صدا بزنه و
           به همون آرایه‌ی segments که هنوز در حال teardown هست
           دسترسی پیدا کنه. */
    }
    LeaveCriticalSection(&eng->lock);

    /* مرحله ۲: خارج از lock، صبر کن segment thread ها واقعاً
       متوقف بشن. حالا اگه طول بکشه، فقط همین تابع (و خود دانلود)
       منتظر می‌مونه، نه کل موتور. */
    if (segs_snapshot) {
        for (uint32_t i = 0; i < seg_count; i++)
            if (segs_snapshot[i]) seg_pause(segs_snapshot[i]);
        free(segs_snapshot);
    }

    /* resume_save بعد از توقف واقعی segment ها — اینطوری
       seg->current مقدار نهایی و دقیق رو منعکس می‌کنه، نه یه
       مقدار میانی که ممکنه هنوز در حال تغییر باشه.
       حیاتی: dl_ref ممکنه dangling شده باشه اگه بین
       LeaveCriticalSection بالا و اینجا، یه thread دیگه (مثلاً
       کاربر که خیلی سریع Remove زده) همین دانلود رو engine_remove
       کرده باشه و حافظه‌ش آزاد شده باشه. باید id رو دوباره با
       engine_find تأیید کنیم تا مطمئن بشیم dl_ref هنوز معتبره —
       این دقیقاً همون use-after-free بود که می‌تونست باعث crash
       کل برنامه روی Pause/Stop بشه. */
    if (was_active && dl_ref) {
        EnterCriticalSection(&eng->lock);
        IDM_Download *still_valid = engine_find(eng, id);
        if (still_valid == dl_ref) {
            /* حالا segment teardown واقعاً تموم شده (چه موفق چه
               zombie) و dl_ref->segments دیگه توسط کس دیگه‌ای در
               حال دستکاری نیست — امن‌ه که واقعاً DL_PAUSED بشه و
               به UI/پلاگین‌ها اطلاع بدیم. این دقیقاً همون نقطه‌ایه
               که قبلاً (اشتباهاً) اول تابع اتفاق می‌افتاد. */
            dl_ref->state = DL_PAUSED;
            resume_save(dl_ref);
            if (eng->notify_hwnd)
                PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                             (WPARAM)id, (LPARAM)DL_PAUSED);
        } else {
            LOGW(LOG_MOD_ENGINE,
                 "dl#%u no longer valid after pause — skip resume_save "
                 "(likely removed concurrently)", id);
        }
        LeaveCriticalSection(&eng->lock);
    }
}

void engine_resume(IDM_Engine *eng, uint32_t id) {
    if (!eng) return;
    EnterCriticalSection(&eng->lock);
    IDM_Download *dl = engine_find(eng, id);
    if (dl && dl->state == DL_PAUSING) {
        /* هنوز در حال pause شدن — segments هنوز توسط خودِ
           engine_pause در حال teardown هست، دست‌زدن بهش از اینجا
           دقیقاً همون race ای بود که باعث crash می‌شد. کاربر باید
           صبر کنه تا واقعاً PAUSED بشه (معمولاً فوری، مگر یه segment
           گیر کرده باشه که تا 10s طول می‌کشه). */
        LOGW(LOG_MOD_ENGINE,
             "dl#%u resume ignored — still pausing, try again shortly", id);
    } else if (dl && (dl->state == DL_PAUSED || dl->state == DL_FAILED)) {
        if (dl->segments && dl->num_segments > 0) {
            dl->resume_segs = (IDM_ResumeSeg *)calloc(dl->num_segments,
                                                       sizeof(IDM_ResumeSeg));
            if (dl->resume_segs) {
                for (uint32_t i = 0; i < dl->num_segments; i++) {
                    IDM_Segment *seg = dl->segments[i];
                    dl->resume_segs[i].id      = seg->id;
                    dl->resume_segs[i].start   = seg->start;
                    dl->resume_segs[i].end     = seg->end;
                    dl->resume_segs[i].current = seg->current;
                }
            }
            bool any_zombie = false;
            for (uint32_t i = 0; i < dl->num_segments; i++)
                if (!seg_destroy(dl->segments[i])) any_zombie = true;
            free(dl->segments);
            dl->segments = NULL;

            if (any_zombie) {
                /* یه segment thread على‌رغم abort هنوز زنده‌ست (سناریوی
                   خیلی نادر). نباید dl->writer رو ببندیم یا دانلود رو
                   دوباره صف کنیم — اون thread ممکنه هنوز داره بهش
                   می‌نویسه. دانلود رو در PAUSED نگه می‌داریم؛ کاربر
                   می‌تونه دوباره Resume بزنه. */
                LOGE(LOG_MOD_ENGINE,
                     "dl#%u has a zombie segment — resume deferred, "
                     "writer left open", id);
                LeaveCriticalSection(&eng->lock);
                return;
            }
        }
        if (dl->writer) { fw_close(dl->writer); dl->writer = NULL; }
        dl->state = DL_QUEUED;
        SetEvent(eng->wake_event);
    }
    LeaveCriticalSection(&eng->lock);
}

void engine_cancel(IDM_Engine *eng, uint32_t id) {
    if (!eng) return;

    IDM_Segment **segs_snapshot = NULL;
    uint32_t      seg_count     = 0;
    IDM_Download *dl_ref        = NULL;

    EnterCriticalSection(&eng->lock);
    IDM_Download *dl = engine_find(eng, id);
    if (dl) {
        bool was_active = (dl->state == DL_DOWNLOADING ||
                           dl->state == DL_LAUNCHING);
        dl->state = DL_CANCELLED;
        dl_ref    = dl;
        seg_count = dl->num_segments;
        if (seg_count > 0 && dl->segments) {
            segs_snapshot = (IDM_Segment **)malloc(
                sizeof(IDM_Segment *) * seg_count);
            if (segs_snapshot) {
                for (uint32_t i = 0; i < seg_count; i++)
                    segs_snapshot[i] = dl->segments[i];
            } else {
                seg_count = 0;
            }
        }
        if (was_active) eng->active_count--;
        if (eng->notify_hwnd)
            PostMessageA(eng->notify_hwnd, IDM_WM_STATE,
                         (WPARAM)id, (LPARAM)DL_CANCELLED);
    }
    LeaveCriticalSection(&eng->lock);

    /* همون fix که در engine_pause اعمال شد — seg_pause بلاک‌شونده
       باید خارج از eng->lock صدا زده بشه تا کل موتور فریز نشه */
    if (segs_snapshot) {
        for (uint32_t i = 0; i < seg_count; i++)
            if (segs_snapshot[i]) seg_pause(segs_snapshot[i]);
        free(segs_snapshot);
    }

    if (dl_ref) {
        EnterCriticalSection(&eng->lock);
        IDM_Download *still_valid = engine_find(eng, id);
        if (still_valid == dl_ref) {
            resume_remove(dl_ref);

            /* باگ واقعی که اینجا فیکس شد: بر خلاف engine_pause/
               engine_resume/engine_remove، این تابع قبلاً بدون چک
               کردن seg->zombie مستقیماً fw_close(dl_ref->writer) رو
               صدا می‌زد. اگه یکی از segment ها به seg_pause بالا
               (۱۰ ثانیه صبر) جواب نده و zombie بشه، worker thread‌ش
               هنوز واقعاً زنده‌ست و می‌تونه هر لحظه fw_write(dl->writer,
               ...) رو صدا بزنه — یعنی یه use-after-free مستقیم روی
               IDM_FileWriter تازه-freeشده (fw_close هم CloseHandle
               می‌کنه هم DeleteCriticalSection هم free(fw)). حالا،
               دقیقاً مثل بقیه‌ی توابع، اگه هر segment ای zombie شده
               باشه writer رو نمی‌بندیم و باز می‌ذاریمش (leak عمدی و
               امن، به‌جای crash). */
            bool any_zombie = false;
            if (dl_ref->segments) {
                for (uint32_t i = 0; i < dl_ref->num_segments; i++) {
                    IDM_Segment *s = dl_ref->segments[i];
                    if (s && s->zombie) { any_zombie = true; break; }
                }
            }
            if (any_zombie) {
                LOGE(LOG_MOD_ENGINE,
                     "dl#%u has a zombie segment after cancel — "
                     "writer left open (leaking) to avoid use-after-free",
                     id);
            } else if (dl_ref->writer) {
                fw_close(dl_ref->writer);
                dl_ref->writer = NULL;
            }
        } else {
            LOGW(LOG_MOD_ENGINE,
                 "dl#%u no longer valid after cancel — skip cleanup "
                 "(likely removed concurrently)", id);
            dl_ref = NULL; /* جلوگیری از استفاده در ادامه */
        }
        LeaveCriticalSection(&eng->lock);

        /* کاربر صراحتاً Cancel کرده (نه Pause) — فایل .tels ناقص
           دیگه به دردی نمی‌خوره، پس پاکش می‌کنیم تا روی دیسک
           جا نمونه. این عملیات فایل‌سیستمی سریعه.
           نکته: dl_ref ممکنه بالا به NULL ست شده باشه اگه دانلود
           همزمان remove شده بود — چک می‌کنیم. */
        if (dl_ref && dl_ref->temp_path[0]) DeleteFileA(dl_ref->temp_path);
    }
}

void engine_remove(IDM_Engine *eng, uint32_t id) {
    if (!eng) return;

    IDM_Segment **segs_snapshot = NULL;
    uint32_t      seg_count     = 0;
    IDM_Download *dl_found      = NULL;

    EnterCriticalSection(&eng->lock);

    /* مرحله ۱: پیدا کردن و unlink کردن از لیست — زیر lock کوتاه.
       seg_destroy (که شامل seg_pause بلاک‌شونده هست) رو هنوز
       صدا نمی‌زنیم، فقط آدرس segment ها رو snapshot می‌گیریم. */
    IDM_Download **pp = &eng->queue_head;
    while (*pp) {
        if ((*pp)->id == id) {
            IDM_Download *dl = *pp;
            *pp = dl->next;

            if (eng->queue_tail == dl) {
                IDM_Download *prev = NULL;
                for (IDM_Download *t = eng->queue_head; t && t->next; t = t->next)
                    prev = t;
                eng->queue_tail = prev;
                if (!eng->queue_head) eng->queue_tail = NULL;
            }

            bool was_active = (dl->state == DL_DOWNLOADING ||
                               dl->state == DL_LAUNCHING);
            if (was_active) eng->active_count--;

            seg_count = dl->num_segments;
            if (seg_count > 0 && dl->segments) {
                segs_snapshot = (IDM_Segment **)malloc(
                    sizeof(IDM_Segment *) * seg_count);
                if (segs_snapshot) {
                    for (uint32_t i = 0; i < seg_count; i++)
                        segs_snapshot[i] = dl->segments[i];
                } else {
                    seg_count = 0;
                }
            }
            dl_found = dl;
            break;
        }
        pp = &(*pp)->next;
    }

    LeaveCriticalSection(&eng->lock);

    /* مرحله ۲: خارج از lock، segment thread ها رو متوقف کن —
       همون fix که در pause/cancel اعمال شد. seg_destroy خودش
       seg_pause (بلاک‌شونده) رو صدا می‌زنه، پس مستقیماً اینجا
       (بدون lock) صداش می‌زنیم. */
    bool any_zombie = false;
    if (segs_snapshot) {
        for (uint32_t i = 0; i < seg_count; i++)
            if (segs_snapshot[i])
                if (!seg_destroy(segs_snapshot[i])) any_zombie = true;
        free(segs_snapshot);
    }

    /* مرحله ۳: آزادسازی باقی منابع dl — این بخش‌ها سریع و
       غیر-blocking هستن، پس زیر lock کوتاه قابل قبوله (هرچند
       چون dl از لیست unlink شده، حتی بدون lock هم امنه چون هیچ
       thread دیگه‌ای دیگه بهش دسترسی نداره) */
    if (dl_found) {
        free(dl_found->segments);
        if (any_zombie) {
            /* یه segment thread على‌رغم abort هنوز زنده‌ست. dl_found
               رو الان آزاد نمی‌کنیم و writer رو نمی‌بندیم — اون
               thread هنوز ممکنه بهشون دسترسی داشته باشه. leak عمدیِ
               کل dl، ترجیح داده‌شده به use-after-free قطعی. */
            LOGE(LOG_MOD_ENGINE,
                 "dl#%u has a zombie segment — leaking IDM_Download, "
                 "writer left open", id);
            return;
        }
        if (dl_found->writer) fw_close(dl_found->writer);
        free(dl_found->resume_segs);
        DeleteCriticalSection(&dl_found->lock);
        free(dl_found);
    }
}

IDM_Download *engine_find(IDM_Engine *eng, uint32_t id) {
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next)
        if (dl->id == id) return dl;
    return NULL;
}

double engine_total_speed(IDM_Engine *eng) {
    if (!eng) return 0.0;
    double total = 0.0;
    EnterCriticalSection(&eng->lock);
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) {
        if (dl->state != DL_DOWNLOADING) continue;
        for (uint32_t i = 0; i < dl->num_segments; i++)
            if (dl->segments && dl->segments[i])
                total += seg_get_speed(dl->segments[i]);
    }
    LeaveCriticalSection(&eng->lock);
    return total;
}

/* ======================================================
   زمان‌بند: تنظیم زمان شروع خودکار
   ====================================================== */
void engine_schedule(IDM_Engine *eng, uint32_t id, int hour, int minute) {
    if (!eng || hour < 0 || hour > 23 || minute < 0 || minute > 59) return;

    EnterCriticalSection(&eng->lock);
    IDM_Download *dl = engine_find(eng, id);
    if (dl) {
        /* محاسبه FILETIME برای امروز در ساعت داده شده */
        SYSTEMTIME st;
        GetLocalTime(&st);
        st.wHour   = (WORD)hour;
        st.wMinute = (WORD)minute;
        st.wSecond = 0;
        st.wMilliseconds = 0;

        FILETIME ft_local, ft_utc;
        SystemTimeToFileTime(&st, &ft_local);
        LocalFileTimeToFileTime(&ft_local, &ft_utc);

        /* اگه زمان گذشته باشه → فردا */
        FILETIME now_ft;
        GetSystemTimeAsFileTime(&now_ft);
        ULARGE_INTEGER t_sched, t_now;
        t_sched.LowPart  = ft_utc.dwLowDateTime;
        t_sched.HighPart = ft_utc.dwHighDateTime;
        t_now.LowPart    = now_ft.dwLowDateTime;
        t_now.HighPart   = now_ft.dwHighDateTime;

        if (t_sched.QuadPart <= t_now.QuadPart) {
            /* یه روز به جلو برو: 1 روز = 864000000000 واحد 100ns */
            t_sched.QuadPart += 864000000000ULL;
            ft_utc.dwLowDateTime  = t_sched.LowPart;
            ft_utc.dwHighDateTime = t_sched.HighPart;
        }

        dl->scheduled_at  = ft_utc;
        dl->has_schedule  = TRUE;

        /* اگه در حال دانلود بود → pause کن */
        if (dl->state == DL_DOWNLOADING || dl->state == DL_LAUNCHING ||
            dl->state == DL_QUEUED) {
            if (dl->state == DL_DOWNLOADING) {
                for (uint32_t i = 0; i < dl->num_segments; i++)
                    if (dl->segments && dl->segments[i])
                        seg_pause(dl->segments[i]);
                eng->active_count--;
            }
            dl->state = DL_PAUSED;
        }

        LOGI(LOG_MOD_ENGINE, "Scheduled #%u at %02d:%02d", id, hour, minute);
    }
    LeaveCriticalSection(&eng->lock);
    SetEvent(eng->wake_event);
}

void engine_unschedule(IDM_Engine *eng, uint32_t id) {
    if (!eng) return;
    EnterCriticalSection(&eng->lock);
    IDM_Download *dl = engine_find(eng, id);
    if (dl) {
        memset(&dl->scheduled_at, 0, sizeof(dl->scheduled_at));
        dl->has_schedule = FALSE;
    }
    LeaveCriticalSection(&eng->lock);
}

/* ======================================================
   صف گروهی: اضافه کردن چند URL از یه رشته با \n
   ====================================================== */

/* ======================================================
   استخراج URL از متن آزاد (clipboard، فایل، متن فرم و ...)
   ======================================================
   قبلاً engine_add_batch فقط روی '\n' جدا می‌کرد و فقط خطی رو
   قبول می‌کرد که *دقیقاً* با "http" یا "ftp" شروع بشه. نتیجه: لینک‌ها
   وقتی با فاصله یا Tab کنار هم بودن، با "\r" تنها جدا شده بودن، توی
   کوتیشن/<> بودن، یا جلوشون متن ("Link: https://...") بود، «No valid
   URLs found» می‌داد — حتی وقتی متن پر از لینک معتبر بود.

   این تابع هر توکنی رو که با http:// https:// ftp:// ftps:// شروع
   بشه پیدا می‌کنه، تا اولین جداکننده (فاصله، کنترل، " < > \ `، یا
   NBSP/zero-width یونیکد) می‌بره، علائم انتهایی جمله (.,;:!?') و
   پرانتز/براکت بدون جفت رو از آخرش برمی‌داره، بایت‌های غیر-ASCII رو
   %XX می‌کنه (تا URL معتبر بمونه)، و لینک‌های تکراری رو حذف می‌کنه.
   خروجی: هر URL در یک خط ('\n'). مقدار برگشتی = تعداد URL یکتا.
   اگه out==NULL فقط می‌شمره. اگه بافر جا نداشته باشه، همون‌جا
   متوقف می‌شه (URL ناقص هرگز نوشته نمی‌شه). */
typedef struct { uint64_t *slots; size_t cap, n; } UrlHashSet;

static bool urlset_insert(UrlHashSet *h, uint64_t v) {
    if (v == 0) v = 1;                      /* 0 = خالی */
    if ((h->n + 1) * 2 > h->cap) {
        size_t ncap = h->cap ? h->cap * 2 : 64;
        uint64_t *ns = (uint64_t *)calloc(ncap, sizeof(uint64_t));
        if (!ns) return true;               /* حافظه نبود: dedup رو رها کن، URL رو نگه دار */
        for (size_t i = 0; i < h->cap; i++) {
            if (!h->slots[i]) continue;
            size_t j = (size_t)h->slots[i] & (ncap - 1);
            while (ns[j]) j = (j + 1) & (ncap - 1);
            ns[j] = h->slots[i];
        }
        free(h->slots);
        h->slots = ns;
        h->cap   = ncap;
    }
    size_t j = (size_t)v & (h->cap - 1);
    while (h->slots[j]) {
        if (h->slots[j] == v) return false; /* تکراری */
        j = (j + 1) & (h->cap - 1);
    }
    h->slots[j] = v;
    h->n++;
    return true;
}

static uint64_t url_fnv1a(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t url_scheme_len(const char *p) {
    if (_strnicmp(p, "https://", 8) == 0) return 8;
    if (_strnicmp(p, "http://",  7) == 0) return 7;
    if (_strnicmp(p, "ftps://",  7) == 0) return 7;
    if (_strnicmp(p, "ftp://",   6) == 0) return 6;
    return 0;
}

/* طول جداکننده‌ی یونیکد (UTF-8) در p؛ صفر اگه نیست: NBSP، U+200B..U+200F،
   U+2028/2029، U+FEFF — این‌ها موقع کپی از وب/چت زیاد بین لینک‌ها میان. */
static size_t url_utf8_sep_len(const unsigned char *p) {
    if (p[0] == 0xC2 && p[1] == 0xA0) return 2;
    if (p[0] == 0xE2 && p[1] == 0x80 &&
        ((p[2] >= 0x8B && p[2] <= 0x8F) || p[2] == 0xA8 || p[2] == 0xA9)) return 3;
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return 3;
    return 0;
}

int engine_extract_urls(const char *text, char *out, size_t out_sz) {
    bool write = (out && out_sz > 0);
    if (write) out[0] = '\0';
    if (!text) return 0;

    UrlHashSet seen = {0};
    size_t pos = 0;
    int    count = 0;
    const char *p = text;

    while (*p) {
        size_t sl = url_scheme_len(p);
        bool boundary = (p == text) || !isalnum((unsigned char)p[-1]);
        if (!sl || !boundary) { p++; continue; }

        /* پایان توکن */
        const char *e = p + sl;
        while (*e) {
            unsigned char c = (unsigned char)*e;
            if (c <= 0x20 || c == 0x7F || c == '"' || c == '<' || c == '>' ||
                c == '\\' || c == '`') break;
            if (c >= 0x80 && url_utf8_sep_len((const unsigned char *)e)) break;
            e++;
        }

        /* علائم انتهایی جمله و براکت‌های بدون جفت */
        while (e > p + sl) {
            char c = e[-1];
            if (c == '.' || c == ',' || c == ';' || c == ':' ||
                c == '!' || c == '?' || c == '\'') { e--; continue; }
            if (c == ')' || c == ']') {
                char open = (c == ')') ? '(' : '[';
                int bal = 0;
                for (const char *q = p; q < e; q++) {
                    if (*q == open) bal++;
                    else if (*q == c) bal--;
                }
                if (bal < 0) { e--; continue; }  /* بسته‌ی اضافه */
            }
            break;
        }

        size_t tlen = (size_t)(e - p);
        if (tlen > sl && urlset_insert(&seen, url_fnv1a(p, tlen))) {
            if (write) {
                /* اندازه‌ی کدگذاری‌شده: هر بایت غیر-ASCII → %XX (۳ بایت) */
                size_t need = 0;
                for (const char *q = p; q < e; q++)
                    need += ((unsigned char)*q >= 0x80) ? 3 : 1;
                if (pos + need + 2 > out_sz) break;    /* جا نیست */
                for (const char *q = p; q < e; q++) {
                    unsigned char c = (unsigned char)*q;
                    if (c >= 0x80) {
                        static const char HEX[] = "0123456789ABCDEF";
                        out[pos++] = '%';
                        out[pos++] = HEX[c >> 4];
                        out[pos++] = HEX[c & 15];
                    } else {
                        out[pos++] = (char)c;
                    }
                }
                out[pos++] = '\n';
                out[pos]   = '\0';
            }
            count++;
        }
        p = e;   /* e همیشه جلوتر از p هست (scheme حداقل ۶ بایته) */
    }

    free(seen.slots);
    return count;
}

int engine_add_batch(IDM_Engine *eng,
                     const char *urls_newline,
                     const char *out_dir,
                     uint32_t    max_segments,
                     uint64_t    speed_limit,
                     const char *group_name) {
    if (!eng || !urls_newline) return 0;

    /* group_id: timestamp برای uniqueness */
    uint32_t gid = (uint32_t)GetTickCount();

    /* ورودی هر متنی می‌تونه باشه (لینک‌ها با فاصله/Tab/\r جدا شده،
       داخل کوتیشن، با متن اطراف ...) — اول URL ها رو تمیز استخراج و
       یکتا می‌کنیم؛ بعد هر کدوم یک خط. */
    size_t in_len  = strlen(urls_newline);
    size_t buf_cap = in_len * 3 + 2;
    char *buf = (char *)malloc(buf_cap);
    if (!buf) return 0;
    int found = engine_extract_urls(urls_newline, buf, buf_cap);
    if (found <= 0) {
        free(buf);
        LOGI(LOG_MOD_ENGINE, "Batch: no http/https/ftp links found in input");
        return 0;
    }

    int count = 0;
    char *line = strtok(buf, "\n");
    while (line) {
        /* trim \r و spaces */
        while (*line == ' ' || *line == '\t') line++;
        int ll = (int)strlen(line);
        while (ll > 0 && (line[ll-1] == '\r' || line[ll-1] == '\n' ||
                          line[ll-1] == ' '))
            line[--ll] = '\0';

        if (ll > 7 && (strncmp(line, "http", 4) == 0 ||
                       strncmp(line, "ftp",  3) == 0)) {
            /* نام فایل از URL */
            const char *fn = strrchr(line, '/');
            const char *fname = (fn && *(fn+1)) ? fn+1 : "download";
            char fname_clean[256] = {0};
            strncpy(fname_clean, fname, sizeof(fname_clean)-1);
            char *q = strchr(fname_clean, '?');
            if (q) *q = '\0';
            if (!fname_clean[0]) snprintf(fname_clean, sizeof(fname_clean), "download_%d", count);

            /* مسیر کامل خروجی */
            char out_path[IDM_MAX_PATH];
            if (out_dir && out_dir[0]) {
                snprintf(out_path, sizeof(out_path), "%s\\%s",
                         out_dir, fname_clean);
            } else {
                GetTempPathA(sizeof(out_path), out_path);
                strncat(out_path, fname_clean,
                        sizeof(out_path) - strlen(out_path) - 1);
            }

            IDM_Download *dl = engine_add(eng, line, out_path,
                                           max_segments, speed_limit);
            if (dl) {
                EnterCriticalSection(&eng->lock);
                dl->group_id = gid;
                if (group_name)
                    snprintf(dl->group_name, sizeof(dl->group_name),
                             "%s", group_name);
                LeaveCriticalSection(&eng->lock);
                count++;
            }
        }
        line = strtok(NULL, "\n");
    }

    free(buf);
    LOGI(LOG_MOD_ENGINE, "Batch added: %d downloads (%d unique link(s) found, group=%u '%s')",
            count, found, gid, group_name ? group_name : "");
    return count;
}

/* ======================================================
   حذف همه completed از صف + لیست UI
   ====================================================== */
int engine_clear_completed(IDM_Engine *eng) {
    if (!eng) return 0;
    int removed = 0;

    EnterCriticalSection(&eng->lock);

    IDM_Download **pp = &eng->queue_head;
    while (*pp) {
        IDM_Download *dl = *pp;
        if (dl->state == DL_COMPLETED || dl->state == DL_CANCELLED) {
            *pp = dl->next;
            if (eng->queue_tail == dl) {
                /* پیدا کردن عنصر ماقبل آخر */
                IDM_Download *prev = NULL;
                for (IDM_Download *t = eng->queue_head; t && t->next; t = t->next)
                    prev = t;
                eng->queue_tail = prev;
                if (!eng->queue_head) eng->queue_tail = NULL;
            }
            bool any_zombie = false;
            for (uint32_t i = 0; i < dl->num_segments; i++)
                if (dl->segments && dl->segments[i])
                    if (!seg_destroy(dl->segments[i])) any_zombie = true;
            free(dl->segments);
            if (any_zombie) {
                LOGE(LOG_MOD_ENGINE,
                     "dl#%u has a zombie segment on clear — leaking, "
                     "writer left open", dl->id);
            } else {
                if (dl->writer) fw_close(dl->writer);
                free(dl->resume_segs);
                DeleteCriticalSection(&dl->lock);
                free(dl);
            }
            removed++;
            /* pp رو تغییر ندیم — *pp الان next بعدیه */
        } else {
            pp = &(*pp)->next;
        }
    }

    LeaveCriticalSection(&eng->lock);

    LOGI(LOG_MOD_ENGINE, "Cleared %d completed download(s)", removed);
    return removed;
}
