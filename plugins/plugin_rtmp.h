/* plugin_rtmp.h
   ─────────────────────────────────────────────────────────────
   RTMP / RTMPS / RTMPE Plugin

   پشتیبانی از:
     rtmp://host/app/stream
     rtmps://host/app/stream  (over TLS)
     rtmpe://host/app/stream  (encrypted)

   روش فنی:
     - RTMP یه پروتکل باینری هستش (Adobe Flash)
     - ما از librtmp (RTMPDump) استفاده نمیکنیم
     - به جاش: capture از output یه process خارجی
       (ffmpeg یا rtmpdump که کاربر نصب کرده)
     - اگه هیچ‌کدوم نبود: خطای واضح به کاربر

   این روش pragmatic هستش:
     - RTMP از صفر ۱۰۰۰۰+ خط کد باینری نیاز داره
     - ffmpeg رایگان و قابل اعتماد هستش
     - کاربر‌های حرفه‌ای معمولاً ffmpeg دارن
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_RTMP_H
#define LASS_PLUGIN_RTMP_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTMP_BACKEND_NONE    = 0,
    RTMP_BACKEND_FFMPEG  = 1,
    RTMP_BACKEND_RTMPDUMP= 2,
} RtmpBackend;

typedef struct {
    RtmpBackend backend;
    char        tool_path[512];  /* مسیر ffmpeg.exe یا rtmpdump.exe */
    BOOL        found;
} RtmpConfig;

/* ── ساخت/نابود ── */
IDM_PluginHooks *rtmp_plugin_create (void);
void             rtmp_plugin_destroy(IDM_PluginHooks *plug);

/* ── تشخیص ── */
BOOL rtmp_is_rtmp_url(const char *url);

/* ── پیدا کردن backend ── */
RtmpConfig rtmp_detect_backend(void);

/* ── دانلود با ffmpeg/rtmpdump ── */
/* out_path: مسیر فایل خروجی (.mp4 یا .flv) */
IDM_Result rtmp_download(IDM_PluginHooks *plug,
                          const char *url,
                          const char *out_path,
                          HWND        notify_hwnd);

/* ── وضعیت ── */
BOOL rtmp_is_downloading(IDM_PluginHooks *plug);
void rtmp_cancel        (IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_RTMP_H */
