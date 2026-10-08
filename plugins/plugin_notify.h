/* plugin_notify.h
   ─────────────────────────────────────────────────────────────
   Download Notification Plugin

   وقتی دانلود تموم/fail میشه یه balloon tooltip از system tray
   یا یه پنجره toast کوچیک نشون میده.

   استفاده:
     notify_plugin_set_tray(plug, g_nid_hwnd, ID_TRAY_ICON);
     notify_plugin_on_complete(plug, "file.zip", TRUE);
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_NOTIFY_H
#define LASS_PLUGIN_NOTIFY_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOTIFY_COMPLETE  = 1,  /* دانلود کامل شد */
    NOTIFY_FAILED    = 2,  /* دانلود fail شد */
    NOTIFY_PAUSED    = 3,  /* دانلود pause شد */
    NOTIFY_BATCH_DONE= 4,  /* همه دانلودهای یه گروه تموم شدن */
    NOTIFY_UNSCANNED = 5,  /* فایل اجرایی دانلود شد — اسکن نشده */
} NotifyEvent;

typedef enum {
    NOTIFY_STYLE_BALLOON = 0,  /* Windows balloon tooltip از tray */
    NOTIFY_STYLE_TOAST   = 1,  /* پنجره toast کوچیک روی صفحه */
} NotifyStyle;

typedef struct {
    BOOL         enabled;
    NotifyStyle  style;
    int          duration_ms;   /* مدت نمایش — 3000ms پیش‌فرض */
    BOOL         sound;         /* صدای اعلان */
} NotifyConfig;

/* ── ساخت/نابود ── */
IDM_PluginHooks *notify_plugin_create (HWND tray_hwnd, UINT tray_id);
void             notify_plugin_destroy(IDM_PluginHooks *plug);

/* ── تنظیمات ── */
void notify_plugin_configure(IDM_PluginHooks *plug, const NotifyConfig *cfg);
void notify_plugin_get_config(IDM_PluginHooks *plug, NotifyConfig *out);

/* ── ارسال notification ── */
void notify_send(IDM_PluginHooks *plug, NotifyEvent evt,
                  const char *filename, const char *detail);

/* ── sync با core: callback که core صدا می‌زنه ── */
void notify_on_download_complete(IDM_PluginHooks *plug,
                                  const char *filename, BOOL success);
void notify_set_tray            (IDM_PluginHooks *plug,
                                  HWND hwnd, UINT id);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_NOTIFY_H */
