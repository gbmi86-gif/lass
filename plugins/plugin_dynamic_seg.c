/* plugin_dynamic_seg.c
   ─────────────────────────────────────────────────────────────
   Dynamic Segment Plugin
   یک thread مانیتور داره که هر ۲ ثانیه segment‌ها رو بررسی می‌کنه:
     ۱. Stall detection → retry
     ۲. Fast segment → split (اگه مجاز باشه)
     ۳. آمار سرعت کلی
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_dynamic_seg.h"
#include "../src/shutdown_brake.h"
#include "../src/lass_log.h"
#include "../src/http.h"

/* ======================================================
   داده خصوصی
   ====================================================== */
typedef struct {
    DynSeg_Config    cfg;
    HANDLE           monitor_thread;
    volatile bool    running;
    CRITICAL_SECTION lock;

    /* لیست دانلودهای فعال که باید مانیتور بشن */
    IDM_Download    *watch_list[64];
    int              watch_count;
} DS_Priv;

/* Global instance */
IDM_PluginHooks *g_ds_plugin_instance = NULL;

/* ======================================================
   اضافه/حذف دانلود از لیست مانیتور
   ====================================================== */
static void ds_watch_add(DS_Priv *priv, IDM_Download *dl) {
    EnterCriticalSection(&priv->lock);
    for (int i = 0; i < priv->watch_count; i++)
        if (priv->watch_list[i] == dl) {
            LeaveCriticalSection(&priv->lock);
            return;
        }
    if (priv->watch_count < 64)
        priv->watch_list[priv->watch_count++] = dl;
    LeaveCriticalSection(&priv->lock);
}

static void ds_watch_remove(DS_Priv *priv, IDM_Download *dl) {
    EnterCriticalSection(&priv->lock);
    for (int i = 0; i < priv->watch_count; i++) {
        if (priv->watch_list[i] == dl) {
            priv->watch_list[i] = priv->watch_list[--priv->watch_count];
            priv->watch_list[priv->watch_count] = NULL;
            break;
        }
    }
    LeaveCriticalSection(&priv->lock);
}

/* ======================================================
   بررسی stall: آیا segment واقعاً stall شده؟

   ── باگ واقعی که اینجا فیکس شد (از روی lass.log واقعی) ──
   نسخه‌ی قبلی این تابع دو مشکل جدی داشت که با هم دقیقاً همون
   الگوی فاجعه‌باری که در لاگ دیده شد رو می‌ساختن (ده‌ها
   "Seg#N stalled -> restart" پشت‌سرهم، تقریباً روی هر دانلود
   واقعی، هر بار فقط چند ثانیه بعد از شروع/split):

   ۱) `timeout_sec` (که از cfg.stall_timeout_sec میاد، پیش‌فرض
      ۳۰ ثانیه) رو کلاً نادیده می‌گرفت: `(void)timeout_sec;`. یعنی
      همین که avg_speed در یه tick تک (هر ۲ ثانیه) زیر ۵۱۲ B/s
      بود، فوراً stall تشخیص داده می‌شد — نه بعد از ۳۰ ثانیه‌ی
      واقعی سرعت پایین.

   ۲) avg_speed یه segment تازه‌ساز/تازه‌resume‌شده دقیقاً ۰.۰
      هست تا وقتی اولین بایت واقعاً برسه (رجوع کن به
      seg_update_speed در segment.c). و `spd < 512.0 && spd >= 0.0`
      این ۰.۰ رو هم قبول می‌کرد! یعنی یه segment که همین الان
      TCP/TLS handshake رو شروع کرده و طبیعتاً هنوز چیزی دریافت
      نکرده، در همون tick بعدی مانیتور (حداکثر ۲ ثانیه بعد) به‌جای
      اینکه فرصت داده بشه، بلافاصله kill+restart می‌شد — قبل از
      اینکه اصلاً شانس رسیدن اولین بایت رو داشته باشه. نتیجه: یه
      حلقه‌ی بی‌پایان restart که segment هیچ‌وقت واقعاً فرصت
      دانلود کردن پیدا نمی‌کرد، دقیقاً چیزی که در لاگ با seg#های
      تازه‌split‌شده که فقط ۴-۲ ثانیه بعد از ساخته شدن دوباره
      "stalled" می‌شدن دیده شد.

   فیکس: الان seg->low_speed_since (در segment.h/c) واقعاً مدت
   زمانی که سرعت زیر آستانه مونده رو track می‌کنه — و فقط بعد از
   timeout_sec واقعی، stall تشخیص داده میشه. هر بار سرعت به بالای
   آستانه برگرده، پنجره صفر میشه (یعنی یه افت لحظه‌ای طبیعی شبکه
   باعث kill نمیشه — فقط سکون واقعاً پایدار). ====================================================== */
static bool seg_is_stalled(IDM_Segment *seg, uint32_t timeout_sec) {
    if (!seg) return false;

    EnterCriticalSection(&seg->lock);
    SEG_State st  = seg->state;
    double    spd = seg->avg_speed;
    bool      stalled = false;

    if (st != SEG_DOWNLOADING) {
        /* حالت‌های دیگه (CONNECTING، RETRYING، ...) منطق backoff
           مخصوص خودشون رو دارن (در segment.c) — این مانیتور فقط
           به SEG_DOWNLOADینگی که واقعاً ساکن مونده کار داره. */
        seg->low_speed_since = 0;
    } else if (spd >= 512.0) {
        seg->low_speed_since = 0; /* سرعت برگشت — پنجره‌ی stall پاک میشه */
    } else {
        DWORD now = GetTickCount();
        if (seg->low_speed_since == 0)
            seg->low_speed_since = now; /* اولین باری که زیر آستانه دیدیمش */
        DWORD low_for_ms = now - seg->low_speed_since;
        if (low_for_ms >= (DWORD)timeout_sec * 1000u)
            stalled = true;
    }
    LeaveCriticalSection(&seg->lock);
    return stalled;
}

/* ======================================================
   تلاش برای split یک segment سریع
   ساده‌ترین روش: یه segment جدید از نیمه باقیمونده
   ====================================================== */
static void try_split(DS_Priv *priv, IDM_Download *dl, IDM_Segment *seg) {
    if (!priv || !dl || !seg) return;

    /* بررسی سقف */
    if (dl->num_segments >= priv->cfg.max_total_segments) return;
    if (!dl->accept_ranges) return;

    EnterCriticalSection(&seg->lock);
    uint64_t cur = seg->current;
    uint64_t end = seg->end;
    double   spd = seg->avg_speed;
    LeaveCriticalSection(&seg->lock);

    /* باید به اندازه کافی باقیمانده داشته باشه */
    if (end == 0 || cur >= end) return;
    uint64_t remaining = end - cur;
    if (remaining < priv->cfg.min_split_size * 2) return;

    /* باید سرعت کافی داشته باشه */
    if (spd < priv->cfg.split_speed_threshold) return;

    /* split point: وسط باقیمانده */
    uint64_t split_at = cur + remaining / 2;

    /* تغییر end این segment */
    EnterCriticalSection(&seg->lock);
    uint64_t old_end = seg->end;
    seg->end = split_at - 1;
    LeaveCriticalSection(&seg->lock);

    /* ── نکته‌ی حیاتی (fix برای crash با ≥3 دانلود همزمان) ──
       dl->segments/dl->num_segments همون آرایه‌ای هستن که کل
       engine.c (scheduler_thread، engine_pause، engine_resume،
       engine_remove، engine_destroy) همیشه زیر eng->lock
       می‌خونه/می‌نویسه. این thread (مانیتور dynamic_seg) قبلاً
       بدون گرفتن هیچ lock ای مستقیم realloc/write روی همین آرایه
       انجام می‌داد — یعنی اگه دقیقاً همون لحظه یه thread دیگه
       (مثلاً engine_pause از یه دکمه‌ی UI، یا خود scheduler_thread)
       داشت dl->segments[i] رو زیر eng->lock می‌خوند، realloc می‌تونست
       بلاک قدیمی رو آزاد/جابه‌جا کنه در حالی که اون thread دیگه
       هنوز داشت از همون آدرس قدیمی می‌خوند → یه use-after-free/
       heap corruption واقعی. احتمال برخورد این race مستقیماً با
       تعداد دانلود/segment همزمان بالا میره — دقیقاً همون الگویی
       که با ۲ دانلود بی‌مشکل و با ۳ تا crash دیده شد. */
    IDM_Engine *eng = dl->eng;
    if (!eng) {
        EnterCriticalSection(&seg->lock);
        seg->end = old_end;
        LeaveCriticalSection(&seg->lock);
        return;
    }

    EnterCriticalSection(&eng->lock);

    /* بازبینی زیر lock — از چک اولیه (بالای تابع) تا اینجا ممکنه
       یه split موازی دیگه یا یه engine_resume/engine_remove این
       عدد رو عوض کرده باشه. */
    if (dl->num_segments >= priv->cfg.max_total_segments) {
        LeaveCriticalSection(&eng->lock);
        EnterCriticalSection(&seg->lock);
        seg->end = old_end;
        LeaveCriticalSection(&seg->lock);
        return;
    }

    /* ساخت segment جدید */
    uint32_t new_id = dl->num_segments;

    /* رشد آرایه segments */
    IDM_Segment **new_arr = (IDM_Segment **)realloc(
        dl->segments,
        (dl->num_segments + 1) * sizeof(IDM_Segment *));
    if (!new_arr) {
        /* rollback */
        LeaveCriticalSection(&eng->lock);
        EnterCriticalSection(&seg->lock);
        seg->end = old_end;
        LeaveCriticalSection(&seg->lock);
        return;
    }
    dl->segments = new_arr;

    IDM_Segment *new_seg = seg_create(new_id, split_at, old_end, dl);
    if (!new_seg) {
        LeaveCriticalSection(&eng->lock);
        EnterCriticalSection(&seg->lock);
        seg->end = old_end;
        LeaveCriticalSection(&seg->lock);
        return;
    }

    dl->segments[dl->num_segments] = new_seg;
    dl->num_segments++;

    LeaveCriticalSection(&eng->lock);

    seg_start(new_seg);

    LOGI("dynamic_seg", "Split seg#%u at %llu -> new seg#%u [%llu-%llu]",
         seg->id, (unsigned long long)split_at,
         new_id,
         (unsigned long long)split_at, (unsigned long long)old_end);
}

/* ======================================================
   Thread مانیتور
   ====================================================== */
static DWORD WINAPI ds_monitor_thread(LPVOID param) {
    DS_Priv *priv = (DS_Priv *)param;
    int brake_id = shutdown_brake_register("dynamic_seg_monitor");

    while (priv->running) {
        Sleep(2000); /* هر ۲ ثانیه */
        if (!priv->running) break;

        EnterCriticalSection(&priv->lock);
        int count = priv->watch_count;
        /* snapshot لیست */
        IDM_Download *list[64];
        memcpy(list, priv->watch_list, count * sizeof(IDM_Download *));
        LeaveCriticalSection(&priv->lock);

        for (int d = 0; d < count; d++) {
            IDM_Download *dl = list[d];
            if (!dl || dl->state != DL_DOWNLOADING) continue;
            if (!dl->segments || !dl->eng) continue;

            /* ── همون fix کنار try_split ──
               dl->segments یه آرایه‌ست که ممکنه هر لحظه توسط
               try_split (همین plugin، thread مانیتور) realloc بشه،
               یا توسط engine.c (پاز/ادامه/حذف) کاملاً جایگزین بشه.
               پیمایش مستقیمش بدون قفل، دقیقاً همون race ای بود که
               باعث crash با ≥۳ دانلود همزمان می‌شد. یه snapshot
               کوتاه زیر eng->lock می‌گیریم، بعد بیرون از lock روش
               کار می‌کنیم (تا مدت نگه‌داشتن lock کم بمونه). */
            IDM_Segment *seg_snapshot[256];
            EnterCriticalSection(&dl->eng->lock);
            uint32_t nseg = dl->num_segments;
            if (nseg > 256) nseg = 256; /* دفاعی — max_total_segments=16 عملاً هرگز به اینجا نمی‌رسه */
            for (uint32_t i = 0; i < nseg; i++)
                seg_snapshot[i] = dl->segments[i];
            LeaveCriticalSection(&dl->eng->lock);

            for (uint32_t i = 0; i < nseg; i++) {
                IDM_Segment *seg = seg_snapshot[i];
                if (!seg) continue;

                /* ── بازبینی زیر lock قبل از هر دست‌کاری‌ای ──
                   seg از یه snapshot میاد که تا ۲ ثانیه (فاصله‌ی
                   تیک این مانیتور) قدیمی می‌تونه باشه — تو همین
                   فاصله ممکنه کاربر دانلود رو pause/remove کرده
                   باشه و seg واقعاً آزاد شده باشه.
                   ⚠️ دفعه‌ی قبل این چک رو فقط جلوی مسیر «stall»
                   گذاشته بودم و مسیر «split» (پایین همین حلقه) رو
                   بدون این محافظت رها کرده بودم — همون سهل‌انگاری
                   دقیقاً باعث crash بعدی (READ در آدرس صفر) شد،
                   چون try_split مستقیم seg->lock رو از یه pointer
                   احتمالاً آزادشده می‌گرفت. الان این چک یه‌بار،
                   قبل از هر دو مسیر، انجام می‌شه. */
                bool still_valid = false;
                EnterCriticalSection(&dl->eng->lock);
                for (uint32_t k = 0; k < dl->num_segments; k++) {
                    if (dl->segments[k] == seg) { still_valid = true; break; }
                }
                LeaveCriticalSection(&dl->eng->lock);
                if (!still_valid) continue;

                /* ── stall detection ── */
                if (seg_is_stalled(seg, priv->cfg.stall_timeout_sec)) {
                    LOGW("dynamic_seg", "Seg#%u stalled (speed=%.0f B/s) -> restart",
                         seg->id, seg->speed);

                    /* ── گفتگو، نه دیکته ──
                       این stall واقعیه (بعد از عبور کامل از
                       stall_timeout_sec، نه یه لحظه‌ی تنها — رجوع
                       کن به فیکس seg_is_stalled بالا). به engine
                       خبر می‌دیم تا دفعه‌ی بعد که یه دانلود تازه از
                       همین host شروع می‌شه، محتاطانه‌تر (segment
                       کمتر) شروع کنه — به‌جای اینکه کورکورانه دوباره
                       همون تعداد اتصال رو به سروری که ثابت کرده
                       دوستش نداره تحمیل کنیم. */
                    if (dl->url[0]) {
                        char host[128];
                        http_extract_host(dl->url, host, sizeof(host));
                        engine_report_host_trouble(host);
                    }

                    /* ── فیکس یه باگ واقعی (علت crash قبلی) ──
                       قبلاً این‌جا مستقیم seg_resume(seg) صدا زده
                       می‌شد، درحالی‌که seg هنوز SEG_DOWNLOADING با
                       یه worker thread کاملاً زنده بود! seg_resume
                       بدون توجه به این، یه CreateThread دیگه روی
                       همون seg می‌زد — یعنی دو thread همزمان روی
                       یه IDM_Segment واحد کار می‌کردن. فیکس درست:
                       اول seg_pause (واقعی و synchronous صبر می‌کنه
                       تا worker قبلی کاملاً تموم بشه)، فقط اگه
                       واقعاً موفق شد (zombie نشد) worker تازه راه
                       بنداز. */
                    seg_pause(seg);
                    if (!seg->zombie)
                        seg_resume(seg);
                    continue;
                }

                /* ── split detection ── */
                EnterCriticalSection(&seg->lock);
                double spd = seg->avg_speed;
                SEG_State st = seg->state;
                LeaveCriticalSection(&seg->lock);

                if (st == SEG_DOWNLOADING &&
                    spd >= priv->cfg.split_speed_threshold) {
                    try_split(priv, dl, seg);
                }
            }
        }
    }
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ======================================================
   Hook callbacks
   ====================================================== */
static void hook_dl_start(IDM_Download *dl) {
    if (!g_ds_plugin_instance) return;
    DS_Priv *priv = (DS_Priv *)g_ds_plugin_instance->priv;
    ds_watch_add(priv, dl);
}

static void hook_dl_done(IDM_Download *dl) {
    if (!g_ds_plugin_instance) return;
    DS_Priv *priv = (DS_Priv *)g_ds_plugin_instance->priv;
    ds_watch_remove(priv, dl);
}

static void hook_dl_pause(IDM_Download *dl) {
    hook_dl_done(dl);
}

static void hook_dl_fail(IDM_Download *dl) {
    hook_dl_done(dl);
}

static int hook_init(void) {
    if (!g_ds_plugin_instance) return -1;
    DS_Priv *priv = (DS_Priv *)g_ds_plugin_instance->priv;
    priv->running = true;
    priv->monitor_thread = CreateThread(NULL, 0, ds_monitor_thread, priv, 0, NULL);
    LOGI("dynamic_seg", "initialized (split_threshold=%.0f B/s, min_size=%llu)",
         priv->cfg.split_speed_threshold,
         (unsigned long long)priv->cfg.min_split_size);
    return 0;
}

void dynamic_seg_plugin_signal_stop(IDM_PluginHooks *plugin) {
    if (!plugin || !plugin->priv) return;
    ((DS_Priv *)plugin->priv)->running = false;
}

bool dynamic_seg_test_is_stalled(IDM_Segment *seg, uint32_t timeout_sec) {
    return seg_is_stalled(seg, timeout_sec);
}

static void hook_destroy(void) {
    if (!g_ds_plugin_instance) return;
    DS_Priv *priv = (DS_Priv *)g_ds_plugin_instance->priv;
    priv->running = false;
    if (priv->monitor_thread) {
        WaitForSingleObject(priv->monitor_thread, 5000);
        CloseHandle(priv->monitor_thread);
        priv->monitor_thread = NULL;
    }
    LOGI("dynamic_seg", "destroyed");
}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *dynamic_seg_plugin_create(const DynSeg_Config *cfg) {
    DS_Priv *priv = (DS_Priv *)calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (cfg) {
        memcpy(&priv->cfg, cfg, sizeof(*cfg));
    } else {
        /* پیش‌فرض‌های معقول */
        priv->cfg.split_speed_threshold = 512.0 * 1024.0; /* 512 KB/s */
        priv->cfg.min_split_size        = 2 * 1024 * 1024; /* 2 MB */
        priv->cfg.stall_timeout_sec     = 30;
        priv->cfg.max_total_segments    = 16;
    }
    InitializeCriticalSection(&priv->lock);

    IDM_PluginHooks *plugin = (IDM_PluginHooks *)calloc(1, sizeof(*plugin));
    if (!plugin) { free(priv); return NULL; }

    plugin->name              = "dynamic_seg";
    plugin->version           = "1.0";
    plugin->api_version       = IDM_PLUGIN_API_VERSION;
    plugin->priv              = priv;
    plugin->on_init           = hook_init;
    plugin->on_destroy        = hook_destroy;
    plugin->on_download_start = hook_dl_start;
    plugin->on_download_done  = hook_dl_done;
    plugin->on_download_pause = hook_dl_pause;
    plugin->on_download_fail  = hook_dl_fail;

    g_ds_plugin_instance = plugin;
    return plugin;
}

void dynamic_seg_plugin_destroy(IDM_PluginHooks *plugin) {
    if (!plugin) return;
    DS_Priv *priv = (DS_Priv *)plugin->priv;
    if (priv) {
        DeleteCriticalSection(&priv->lock);
        free(priv);
    }
    if (g_ds_plugin_instance == plugin)
        g_ds_plugin_instance = NULL;
    free(plugin);
}

int dynamic_seg_test_watch_count(void) {
    if (!g_ds_plugin_instance || !g_ds_plugin_instance->priv) return -1;
    return ((DS_Priv *)g_ds_plugin_instance->priv)->watch_count;
}
