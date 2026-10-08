/* plugin_speed_graph.h
   ─────────────────────────────────────────────────────────────
   Speed Graph Plugin – ثبت و نمایش نمودار سرعت دانلود

   ویژگی‌ها:
     - ثبت سرعت لحظه‌ای هر ثانیه (ring buffer)
     - رسم نمودار با GDI در یک پنجره مجزا
     - نمایش peak/average/current
     - قابل embed در پنجره اصلی یا float

   استفاده در main.c:
     #include "../plugins/plugin_speed_graph.h"
     ...
     g_sg_plugin = speed_graph_plugin_create(g_hwnd);
     plugin_attach(g_registry, g_sg_plugin);
     ...
     // برای نمایش/مخفی‌کردن پنجره نمودار:
     speed_graph_show(g_sg_plugin, TRUE);
     ...
     speed_graph_plugin_destroy(g_sg_plugin);
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_SPEED_GRAPH_H
#define IDM_PLUGIN_SPEED_GRAPH_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── تنظیمات ─────────────────────────────────────────── */
#define SG_HISTORY_SECONDS   60   /* نمودار ۶۰ ثانیه اخیر */
#define SG_SAMPLE_INTERVAL_MS 1000 /* نمونه‌گیری هر ۱ ثانیه */
/* اندازه‌ی پنجره‌ی شناور — قبلاً 400x140 بود، بعد 220x90؛ کاربر
   با مقایسه‌ی چند دانلود منیجر دیگه (Download Accelerator Plus،
   IDM) گفت پنجره‌ی شناور اونا معمولاً نصف اندازه‌ی ماست. اومدیم
   پایین‌تر (160x64) — ولی چون این یه گراف کامل با محور و برچسبه
   (نه یه sparkline خام مثل خیلی از رقبا)، فقط کوچیک کردن پنجره
   کافی نبود؛ margin ها هم متناسب کوچیک شدن (پایین‌تر) و تعداد
   خط‌های grid/برچسب هم کم شد (sg_draw در plugin_speed_graph.c)
   تا توی این فضای کوچیک‌تر شلوغ/غیرقابل‌خوندن نشه. */
#define SG_WINDOW_W          160
#define SG_WINDOW_H           64
#define SG_GRAPH_MARGIN_L     40
#define SG_GRAPH_MARGIN_R      6
#define SG_GRAPH_MARGIN_T      6
#define SG_GRAPH_MARGIN_B     16

/* ── ساخت/نابود ─────────────────────────────────────── */
IDM_PluginHooks *speed_graph_plugin_create(HWND parent_hwnd,
                                            IDM_Engine *engine);
void             speed_graph_plugin_destroy(IDM_PluginHooks *plug);

/* فقط timer_running رو FALSE می‌کنه (بدون wait/close) — برای صدا
   زدن قبل از shutdown_brake_wait_all، رجوع کن به توضیح در
   plugin_dynamic_seg.h / main.c. */
void             speed_graph_plugin_signal_stop(IDM_PluginHooks *plug);

/* ── کنترل پنجره ────────────────────────────────────── */
void speed_graph_show  (IDM_PluginHooks *plug, BOOL visible);
BOOL speed_graph_visible(IDM_PluginHooks *plug);

/* ── دسترسی به داده (برای ادغام با GUI اصلی) ─────────── */
typedef struct {
    double current_bps;   /* سرعت لحظه‌ای */
    double peak_bps;      /* بیشترین سرعت */
    double avg_bps;       /* میانگین */
    double samples[SG_HISTORY_SECONDS];
    int    sample_count;  /* تعداد معتبر */
    int    head;          /* ایندکس ring buffer */
} SG_Stats;

/* داده‌های آماری رو پر می‌کنه – thread-safe */
void speed_graph_get_stats(IDM_PluginHooks *plug, SG_Stats *out);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_SPEED_GRAPH_H */
