/* plugin_browser_ext.h
   ─────────────────────────────────────────────────────────────
   Browser Extension Integration Plugin

   معماری:
     Chrome/Firefox Extension
           ↓  Native Messaging (stdin/stdout JSON)
     plugin_browser_ext (Named Pipe server)
           ↓
     engine_add() / IDM_WM_ADDED

   پروتکل:
     Extension → Lass:
       {"action":"download","url":"...","filename":"...","referer":"..."}
       {"action":"ping"}

     Lass → Extension:
       {"status":"ok","id":42}
       {"status":"pong","version":"5.0"}

   ثبت:
     HKCU\Software\Google\Chrome\NativeMessagingHosts\com.lass.downloadmanager
     HKCU\Software\Mozilla\NativeMessagingHosts\com.lass.downloadmanager
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_BROWSER_EXT_H
#define LASS_PLUGIN_BROWSER_EXT_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LASS_EXT_HOST_NAME  "com.lass.downloadmanager"
#define LASS_PIPE_NAME      "\\\\.\\pipe\\LassNativeHost"

typedef enum {
    BROWSER_NONE    = 0,
    BROWSER_CHROME  = 1,
    BROWSER_FIREFOX = 2,
    BROWSER_EDGE    = 4,
    BROWSER_ALL     = 7,
} BrowserType;

/* callback وقتی extension درخواست دانلود می‌فرسته */
typedef void (*ExtDownloadCb)(const char *url,
                               const char *filename,
                               const char *referer,
                               void       *userdata);

/* ── ساخت/نابود ── */
IDM_PluginHooks *browser_ext_plugin_create(ExtDownloadCb on_download,
                                            void          *userdata);
void             browser_ext_plugin_destroy(IDM_PluginHooks *plug);

/* فقط p->running رو FALSE می‌کنه و یه dummy client به pipe وصل
   می‌کنه تا server_thread از ConnectNamedPipe بلاک‌شده بیرون بیاد —
   بدون WaitForSingleObject/CloseHandle. برای صدا زدن قبل از
   shutdown_brake_wait_all؛ رجوع کن به توضیح در main.c. */
void             browser_ext_plugin_signal_stop(IDM_PluginHooks *plug);

/* ── ثبت native messaging host در registry ── */
BOOL browser_ext_register  (BrowserType browsers);
BOOL browser_ext_unregister(BrowserType browsers);
BOOL browser_ext_is_registered(BrowserType browser);
BOOL browser_ext_diagnose(IDM_PluginHooks *plug, BrowserType browser,
                          char *out_msg, int out_sz);
BOOL browser_ext_is_browser_installed(BrowserType browser);
void browser_ext_get_extension_dir(char *out, int out_sz);

/* ── وضعیت ── */
BOOL browser_ext_is_running(IDM_PluginHooks *plug);
int  browser_ext_client_count(IDM_PluginHooks *plug);

/* تعداد پیام‌هایی که سرور کامل خونده/parse کرده (صرف‌نظر از نوع یا
   موفقیت dispatch) — برای عیب‌یابی: مشخص می‌کنه پیام اصلاً به سرور
   رسیده یا نه، مستقل از موفقیت callback یا وضعیت لاگ. */
long browser_ext_messages_received(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_BROWSER_EXT_H */
