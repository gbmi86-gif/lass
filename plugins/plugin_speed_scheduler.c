/* plugin_speed_scheduler.c — برنامه‌ریزی سرعت بر اساس ساعت
   ─────────────────────────────────────────────────────────
   اصول طراحی امن (مطابق فلسفه‌ی پروژه: ایمنی قبل از سرعت):

   ۱. polling thread فقط GetLocalTime می‌خونه و یه عدد به
      engine_set_global_speed_limit می‌فرسته — هیچ pointer
      دیگه‌ای لمس نمیشه.

   ۲. تمام دسترسی به config زیر CRITICAL_SECTION هست.

   ۳. هنگام destroy، اول limit صفر میشه (unlimited) بعد thread
      با timeout متوقف میشه — این ترتیب عمدیه: حتی اگه destroy
      وسط یه iteration گیر کنه، دانلودها هیچ‌وقت با یه limit
      قدیمی "گیر" نمی‌مونن.

   ۴. از همون pattern thread-handle-tracking که در browser_ext
      برای جلوگیری از crash استفاده شد، استفاده میشه.
   ─────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_speed_scheduler.h"
#include "../src/lass_log.h"
#include "../src/shutdown_brake.h"

#define SPEED_SCHED_POLL_MS 15000  /* هر ۱۵ ثانیه چک کن — کافیه،
                                       نیازی به دقت ثانیه‌ای نیست */

typedef struct {
    DWORD            magic;
    IDM_Engine      *eng;          /* فقط برای صدا زدن setter — هیچ‌وقت
                                       مستقیم دسترسی به فیلدهاش نداریم */
    SpeedSchedConfig cfg;
    CRITICAL_SECTION lock;

    HANDLE           poll_thread;
    volatile BOOL    running;
} SS_Priv;

#define SS_MAGIC 0x53534C50u /* "SSLP" ASCII */

/* ── polling thread ── */
static DWORD WINAPI ss_poll_thread(LPVOID param) {
    SS_Priv *p = (SS_Priv *)param;
    int brake_id = shutdown_brake_register("speed_scheduler");

    while (p->running) {
        EnterCriticalSection(&p->lock);
        BOOL enabled = p->cfg.enabled;
        uint64_t limit = 0;
        if (enabled) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            int hour = (int)st.wHour;
            if (hour >= 0 && hour < SPEED_SCHED_HOURS)
                limit = p->cfg.limit_per_hour[hour];
        }
        LeaveCriticalSection(&p->lock);

        /* فقط یه عدد به engine — کوتاه‌ترین تماس ممکن */
        engine_set_global_speed_limit(p->eng, limit);

        /* خواب کوتاه با چک مکرر running تا shutdown سریع باشه */
        for (int i = 0; i < (SPEED_SCHED_POLL_MS / 200) && p->running; i++)
            Sleep(200);
    }
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ── lifecycle ── */
IDM_PluginHooks *speed_sched_plugin_create(IDM_Engine *eng) {
    if (!eng) return NULL;

    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;

    SS_Priv *p = (SS_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    p->magic = SS_MAGIC;
    p->eng   = eng;
    InitializeCriticalSection(&p->lock);

    /* پیش‌فرض: غیرفعال، همه ساعت‌ها نامحدود */
    p->cfg.enabled = FALSE;
    memset(p->cfg.limit_per_hour, 0, sizeof(p->cfg.limit_per_hour));

    p->running = TRUE;
    p->poll_thread = CreateThread(NULL, 0, ss_poll_thread, p, 0, NULL);
    if (!p->poll_thread) {
        p->running = FALSE;
        DeleteCriticalSection(&p->lock);
        free(p);
        free(plug);
        return NULL;
    }

    plug->priv        = p;
    plug->name        = "speed_scheduler";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init      = NULL;
    plug->on_destroy   = NULL;

    LOGI("speed_scheduler", "Plugin created");
    return plug;
}

void speed_sched_plugin_signal_stop(IDM_PluginHooks *plug) {
    if (!plug || !plug->priv) return;
    SS_Priv *p = (SS_Priv *)plug->priv;
    if (p->magic != SS_MAGIC) return;
    p->running = FALSE;
}

void speed_sched_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    SS_Priv *p = (SS_Priv *)plug->priv;
    if (p) {
        if (p->magic != SS_MAGIC) {
            LOGE("speed_scheduler", "FATAL: magic mismatch — heap corruption, skip cleanup");
            free(plug);
            return;
        }

        /* اول limit رو صفر کن — اگه destroy وسط یه iteration گیر
           کنه، هیچ دانلودی با محدودیت قدیمی "گیر" نمی‌مونه */
        engine_set_global_speed_limit(p->eng, 0);

        p->running = FALSE;
        if (p->poll_thread) {
            WaitForSingleObject(p->poll_thread, 3000);
            CloseHandle(p->poll_thread);
        }

        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}

/* ── config API ── */
void speed_sched_set_config(IDM_PluginHooks *plug, const SpeedSchedConfig *cfg) {
    if (!plug || !cfg) return;
    SS_Priv *p = (SS_Priv *)plug->priv;
    if (!p || p->magic != SS_MAGIC) return;

    EnterCriticalSection(&p->lock);
    p->cfg = *cfg;
    LeaveCriticalSection(&p->lock);

    LOGI("speed_scheduler", "Config updated (enabled=%s)",
         cfg->enabled ? "yes" : "no");
}

BOOL speed_sched_get_config(IDM_PluginHooks *plug, SpeedSchedConfig *out) {
    if (!plug || !out) return FALSE;
    SS_Priv *p = (SS_Priv *)plug->priv;
    if (!p || p->magic != SS_MAGIC) return FALSE;

    EnterCriticalSection(&p->lock);
    *out = p->cfg;
    LeaveCriticalSection(&p->lock);
    return TRUE;
}

void speed_sched_set_range(SpeedSchedConfig *cfg, int hour_start, int hour_end,
                            uint64_t bytes_per_sec) {
    if (!cfg) return;
    if (hour_start < 0) hour_start = 0;
    if (hour_end >= SPEED_SCHED_HOURS) hour_end = SPEED_SCHED_HOURS - 1;
    for (int h = hour_start; h <= hour_end; h++)
        cfg->limit_per_hour[h] = bytes_per_sec;
}

uint64_t speed_sched_get_current_limit(IDM_PluginHooks *plug) {
    if (!plug) return 0;
    SS_Priv *p = (SS_Priv *)plug->priv;
    if (!p || p->magic != SS_MAGIC) return 0;

    EnterCriticalSection(&p->lock);
    BOOL enabled = p->cfg.enabled;
    uint64_t limit = 0;
    if (enabled) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        int hour = (int)st.wHour;
        if (hour >= 0 && hour < SPEED_SCHED_HOURS)
            limit = p->cfg.limit_per_hour[hour];
    }
    LeaveCriticalSection(&p->lock);
    return limit;
}

BOOL speed_sched_is_enabled(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    SS_Priv *p = (SS_Priv *)plug->priv;
    if (!p || p->magic != SS_MAGIC) return FALSE;
    EnterCriticalSection(&p->lock);
    BOOL e = p->cfg.enabled;
    LeaveCriticalSection(&p->lock);
    return e;
}
