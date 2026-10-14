/* plugin_dynamic_seg.h
   ─────────────────────────────────────────────────────────────
   پلاگین ۲: Dynamic Segment
   - مانیتور سرعت هر segment
   - اگه یه segment خیلی سریعه و باقیمونده زیاد → split
   - اگه segment‌ها کند/stalled شدن → retry
   - کاملاً مستقل از کد اصلی
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_DYNAMIC_SEG_H
#define IDM_PLUGIN_DYNAMIC_SEG_H

#include "plugin_api.h"

typedef struct {
    double   split_speed_threshold; /* B/s – اگه سرعت بالاتر بود split کن */
    uint64_t min_split_size;        /* حداقل بایت برای split (پیش‌فرض: 2MB) */
    uint32_t stall_timeout_sec;     /* ثانیه بی‌فعالی = stalled (پیش‌فرض: 30) */
    uint32_t max_total_segments;    /* سقف کل segment‌ها (پیش‌فرض: 16) */
} DynSeg_Config;

IDM_PluginHooks *dynamic_seg_plugin_create(const DynSeg_Config *cfg);
void             dynamic_seg_plugin_destroy(IDM_PluginHooks *plugin);

/* فقط priv->running رو false می‌کنه — بدون WaitForSingleObject/
   CloseHandle. برای صدا زدن قبل از shutdown_brake_wait_all، تا
   ds_monitor_thread زودتر (نه بعد از تایم‌اوت بی‌فایده‌ی brake)
   خبردار بشه باید خارج بشه. dynamic_seg_plugin_destroy بعداً
   (بعد از wait) صدا زده میشه و چون thread احتمالاً از قبل تموم
   شده، سریع/بدون تأخیر عمل می‌کنه. */
void             dynamic_seg_plugin_signal_stop(IDM_PluginHooks *plugin);

/* فقط برای تست: تعداد دانلودهای فعلاً watch‌شده توسط مانیتور */
int              dynamic_seg_test_watch_count(void);

/* ── فقط برای تست ──
   wrapper صادرشده‌ی seg_is_stalled (که در plugin_dynamic_seg.c
   static هست) تا test_all.c بتونه دقیقاً همون منطق واقعی
   stall-detection رو، با IDM_Segment های دستی‌ساز، مستقیم تست کنه
   — به‌جای اینکه فقط به «کرش نکرد» قناعت کنه. هیچ‌جای دیگه‌ی
   برنامه نباید از این استفاده کنه. */
bool             dynamic_seg_test_is_stalled(IDM_Segment *seg, uint32_t timeout_sec);

#endif /* IDM_PLUGIN_DYNAMIC_SEG_H */
