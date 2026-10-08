/* plugin_speed_scheduler.h — برنامه‌ریزی سرعت بر اساس ساعت
   ─────────────────────────────────────────────────────────
   هدف: کاربر می‌تونه برای هر ساعت از شبانه‌روز یه سقف سرعت
   تعریف کنه (مثلاً ۸ صبح تا ۶ عصر = ۵۱۲ KB/s، بقیه = نامحدود).

   طراحی امن:
   - این پلاگین فقط به یه عدد global (g_global_speed_limit در
     engine.c) می‌نویسه، هیچ دسترسی مستقیمی به IDM_Download،
     IDM_Segment، یا هر pointer داخلی engine نداره.
   - polling thread هر بار فقط GetLocalTime می‌خونه و یه عدد
     می‌نویسه — کوتاه‌ترین و کم‌خطرترین عملیات ممکن.
   - اگه پلاگین destroy بشه، خود به خود limit رو صفر می‌کنه
     (یعنی unlimited) تا هیچ‌وقت یه download بی‌دلیل گیر نکنه.
   ─────────────────────────────────────────────────────────
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "plugin_api.h"
#include "../src/engine.h"

#define SPEED_SCHED_HOURS 24

typedef struct {
    BOOL     enabled;                      /* فعال/غیرفعال کل سیستم */
    uint64_t limit_per_hour[SPEED_SCHED_HOURS]; /* bytes/sec، 0=نامحدود */
} SpeedSchedConfig;

IDM_PluginHooks *speed_sched_plugin_create(IDM_Engine *eng);
void             speed_sched_plugin_destroy(IDM_PluginHooks *plug);

/* فقط p->running رو FALSE می‌کنه (بدون wait/close/free) — برای صدا
   زدن قبل از shutdown_brake_wait_all. poll_thread هر ۲۰۰ms خودش
   این پرچم رو چک می‌کنه، پس بعد از این صدا خیلی زود تمومش می‌کنه. */
void             speed_sched_plugin_signal_stop(IDM_PluginHooks *plug);

/* تنظیم/خواندن کانفیگ — thread-safe (CRITICAL_SECTION داخلی) */
void speed_sched_set_config(IDM_PluginHooks *plug, const SpeedSchedConfig *cfg);
BOOL speed_sched_get_config(IDM_PluginHooks *plug, SpeedSchedConfig *out);

/* یه helper ساده: یه ساعت رو با یه سقف یکسان پر کن */
void speed_sched_set_range(SpeedSchedConfig *cfg, int hour_start, int hour_end,
                            uint64_t bytes_per_sec);

/* محدودیت فعلی بر اساس ساعت الان رو برمی‌گردونه (برای UI/debug) */
uint64_t speed_sched_get_current_limit(IDM_PluginHooks *plug);

/* null-safe: همیشه قابل فراخوانی، حتی با NULL */
BOOL speed_sched_is_enabled(IDM_PluginHooks *plug);
