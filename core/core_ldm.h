/* core_ldm.h
   ─────────────────────────────────────────────────────────────
   Lass Download Manager – Core API

   این header تنها نقطه تماس بین GUO (UI) و هسته هستش.
   GUO هرگز مستقیم به engine.h، segment.h و غیره دسترسی نداره.

   جریان:
     GUO → core_ldm_dispatch(event) → engine/plugins
     engine → core_ldm_notify(state) → GUO callback

   مزایا:
     - GUO قابل تعویض هستش (SDL2، Qt، headless)
     - core قابل تست بدون GUI هستش
     - پلاگین‌ها فقط plugin_api.h رو می‌بینن
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_CORE_LDM_H
#define LASS_CORE_LDM_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "../src/settings.h"   /* LassSettings — باید قبل از استفاده include بشه */

#ifdef __cplusplus
extern "C" {
#endif

/* ── forward ── */
typedef struct LDM_Core  LDM_Core;
typedef struct LDM_Item  LDM_Item;

/* ======================================================
   رویدادهایی که GUO به core می‌فرسته
   ====================================================== */
typedef enum {
    LDM_EVT_ADD = 1,        /* اضافه کردن دانلود */
    LDM_EVT_ADD_BATCH,      /* اضافه کردن دسته‌ای */
    LDM_EVT_PAUSE,          /* توقف */
    LDM_EVT_RESUME,         /* ادامه */
    LDM_EVT_CANCEL,         /* لغو */
    LDM_EVT_REMOVE,         /* حذف از لیست */
    LDM_EVT_CLEAR_DONE,     /* پاک کردن تمام‌شده‌ها */
    LDM_EVT_SCHEDULE,       /* زمان‌بندی */
    LDM_EVT_SETTINGS_SAVE,  /* ذخیره تنظیمات */
    LDM_EVT_QUIT,           /* خروج */
} LDM_EventType;

/* داده همراه رویداد ADD */
typedef struct {
    char     url[2048];
    char     out_path[512];
    uint32_t segments;
    uint64_t speed_limit;
    /* اختیاری — برای سایت‌هایی که بدون Referer معتبر رد می‌کنن
       (خالی = هیچ Referer header‌ای فرستاده نمیشه) */
    char     referer[2048];
} LDM_AddParams;

/* داده همراه رویداد ADD_BATCH */
typedef struct {
    char    *urls_newline;   /* malloc'd — core free می‌کنه */
    char     out_dir[512];
    char     group_name[64];
    uint32_t segments;
} LDM_BatchParams;

/* داده همراه رویداد SCHEDULE */
typedef struct {
    uint32_t id;
    int      hour;
    int      minute;
} LDM_ScheduleParams;

typedef union {
    LDM_AddParams      add;
    LDM_BatchParams    batch;
    LDM_ScheduleParams schedule;
    uint32_t           id;     /* برای PAUSE / RESUME / CANCEL / REMOVE */
} LDM_EventData;

typedef struct {
    LDM_EventType type;
    LDM_EventData data;
} LDM_Event;

/* ======================================================
   وضعیت‌هایی که core به GUO گزارش میده
   ====================================================== */
typedef enum {
    LDM_NOTIFY_ADDED = 1,    /* دانلود جدید اضافه شد */
    LDM_NOTIFY_PROGRESS,     /* پیشرفت دانلود */
    LDM_NOTIFY_STATE,        /* تغییر state */
    LDM_NOTIFY_REMOVED,      /* دانلود حذف شد */
    LDM_NOTIFY_CLEARED,      /* چند تا حذف شد */
    LDM_NOTIFY_EXT_URL,      /* browser extension یه URL فرستاد */
    LDM_NOTIFY_CLIP_URL,     /* clipboard یه URL تشخیص داد */
} LDM_NotifyType;

/* snapshot یه دانلود برای نمایش — بدون pointer به engine internals */
typedef struct LDM_Item {
    uint32_t id;
    char     url[2048];
    char     filename[256];
    char     out_path[512];
    int      state;         /* DL_State از types.h */
    uint64_t total_size;
    int64_t  downloaded;
    double   speed_bps;
    int      progress_pct;  /* 0-100 */
    uint32_t group_id;
    char     group_name[64];
    bool     has_schedule;
    int      sched_hour;
    int      sched_minute;
    char     fail_reason[96]; /* دلیل خوانا برای Failed، خالی اگه Failed نیست */
} LDM_Item;

typedef struct {
    LDM_NotifyType type;
    uint32_t       id;       /* id دانلود مربوطه */
    LDM_Item       item;     /* snapshot — فقط برای ADDED و STATE */
    int            count;    /* برای CLEARED: تعداد حذف‌شده */
    char           url[2048]; /* برای EXT_URL و CLIP_URL */
    char           referer[2048]; /* فقط برای EXT_URL — خالی یعنی نداریم */
} LDM_Notify;

/* callback که GUO ثبت می‌کنه */
typedef void (*LDM_NotifyCb)(const LDM_Notify *n, void *userdata);

/* ======================================================
   آمار کلی — برای statusbar و speed graph
   ====================================================== */
typedef struct {
    int    active_count;
    int    queued_count;
    int    total_count;
    double total_speed_bps;
    double peak_speed_bps;
} LDM_Stats;

/* ======================================================
   API هسته
   ====================================================== */

/* ── راه‌اندازی / خاموش کردن ── */
LDM_Core *core_create (HWND notify_hwnd);
void      core_destroy(LDM_Core *core);

/* فقط سیگنال «شروع کن به خاموش شدن» (پرچم‌ها/SetEvent) — بدون هیچ
   wait/join/free. باید *قبل* از shutdown_brake_wait_all در
   main.c صدا زده بشه؛ core_destroy طبق معمول بعد از اون wait صدا
   زده میشه و کار join واقعی + آزادسازی رو انجام می‌ده. */
void      core_signal_shutdown(LDM_Core *core);

/* ── ثبت callback برای دریافت notificationها ── */
void core_set_notify_cb(LDM_Core *core, LDM_NotifyCb cb, void *userdata);

/* ── ارسال رویداد از GUO به core ── */
void core_dispatch(LDM_Core *core, const LDM_Event *evt);

/* ── shorthand helpers (ساخت و dispatch یه رویداد) ── */
void core_add     (LDM_Core *core, const char *url, const char *path,
                   uint32_t segs, uint64_t speed_limit);
/* مثل core_add، به‌علاوه‌ی Referer اختیاری (NULL/"" = بدون Referer) —
   برای سایت‌هایی که بدون Referer معتبر با ۴۰۳ رد می‌کنن. */
void core_add_ex  (LDM_Core *core, const char *url, const char *path,
                   uint32_t segs, uint64_t speed_limit,
                   const char *referer);
void core_batch   (LDM_Core *core, const char *urls, const char *dir,
                   const char *group);
int  core_batch_count(LDM_Core *core, const char *urls, const char *dir,
                      const char *group);
void core_pause   (LDM_Core *core, uint32_t id);
void core_resume  (LDM_Core *core, uint32_t id);
void core_cancel  (LDM_Core *core, uint32_t id);
void core_remove  (LDM_Core *core, uint32_t id);
int  core_clear_done(LDM_Core *core);
void core_schedule(LDM_Core *core, uint32_t id, int hour, int minute);

/* ── خواندن وضعیت (snapshot) ── */
LDM_Stats core_get_stats(LDM_Core *core);
bool      core_get_item (LDM_Core *core, uint32_t id, LDM_Item *out);

/* ── fill کردن لیست همه آیتم‌ها ── */
/* out_items: آرایه از caller، max_items: ظرفیت، برمیگردونه تعداد پر شده */
int core_list_items(LDM_Core *core, LDM_Item *out_items, int max_items);

/* ── دسترسی به engine برای پلاگین‌ها (read-only) ── */
void *core_get_engine(LDM_Core *core); /* returns IDM_Engine* */

/* ── settings ── */
void core_settings_apply(LDM_Core *core, const LassSettings *s);

/* ── proxy / auth پلاگین‌ها ── */
void core_set_proxy(LDM_Core *core, const char *proxy_url, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* LASS_CORE_LDM_H */
