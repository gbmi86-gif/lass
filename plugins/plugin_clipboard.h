/* plugin_clipboard.h
   ─────────────────────────────────────────────────────────────
   Clipboard Monitor Plugin – پایش کلیپ‌بورد برای لینک دانلود

   وقتی کاربر یه URL (http/https/ftp) کپی می‌کنه،
   پلاگین اون رو تشخیص داده و:
     1. به GUI اطلاع می‌ده (callback)
     2. اختیاری: مستقیماً دانلود شروع می‌کنه

   استفاده در main.c:
     g_cb_plugin = clipboard_plugin_create(g_hwnd, on_url_detected);
     plugin_attach(g_registry, g_cb_plugin);
     ...
     clipboard_plugin_set_enabled(g_cb_plugin, TRUE);
     ...
     clipboard_plugin_destroy(g_cb_plugin);
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_CLIPBOARD_H
#define IDM_PLUGIN_CLIPBOARD_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* callback وقتی URL جدید در کلیپ‌بورد تشخیص داده شد */
typedef void (*CB_UrlDetected)(const char *url, void *userdata);

/* ── ساخت/نابود ─────────────────────────────────────── */
IDM_PluginHooks *clipboard_plugin_create(HWND        owner_hwnd,
                                          CB_UrlDetected on_url,
                                          void          *userdata);
void             clipboard_plugin_destroy(IDM_PluginHooks *plug);

/* ── کنترل ──────────────────────────────────────────── */
void clipboard_plugin_set_enabled(IDM_PluginHooks *plug, BOOL enabled);
BOOL clipboard_plugin_is_enabled (IDM_PluginHooks *plug);

/* آخرین URL تشخیص‌داده‌شده (thread-safe) */
void clipboard_plugin_get_last_url(IDM_PluginHooks *plug,
                                    char *out, int max_len);

/* فیلتر پسوند: فقط URLهایی که به این پسوندها ختم میشن رو گزارش بده
   exts مثلاً: ".zip\0.exe\0.mp4\0" (null-separated, double-null end)
   اگه NULL بده = همه URLها */
void clipboard_plugin_set_filter(IDM_PluginHooks *plug, const char *exts);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_CLIPBOARD_H */
