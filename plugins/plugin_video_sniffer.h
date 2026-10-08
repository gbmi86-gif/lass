/* plugin_video_sniffer.h
   ─────────────────────────────────────────────────────────────
   Lass – Video Sniffer Plugin

   وقتی browser extension یه URL از یه سایت ویدیویی می‌فرسته،
   این پلاگین کیفیت‌های مختلف رو شناسایی می‌کنه و به GUO
   یه لیست برای انتخاب کاربر می‌فرسته.

   پشتیبانی:
     - YouTube  (از ytdl-compatible API)
     - آپارات   (aparat.com)
     - اینستاگرام (instagram.com)
     - توییتر/X  (twitter.com / x.com)
     - URL مستقیم HLS / DASH
     - هر لینک mp4/mkv/avi مستقیم
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_VIDEO_SNIFFER_H
#define LASS_PLUGIN_VIDEO_SNIFFER_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VS_MAX_QUALITIES   16
#define VS_MAX_URL        2048
#define VS_MAX_LABEL        64

typedef enum {
    VS_SRC_UNKNOWN    = 0,
    VS_SRC_DIRECT     = 1,  /* لینک mp4/mkv/... مستقیم */
    VS_SRC_HLS        = 2,  /* M3U8 */
    VS_SRC_DASH       = 3,  /* MPD */
    VS_SRC_YOUTUBE    = 4,
    VS_SRC_APARAT     = 5,
    VS_SRC_INSTAGRAM  = 6,
    VS_SRC_TWITTER    = 7,
    VS_SRC_VIMEO      = 8,
    VS_SRC_GENERIC    = 9,  /* سایت‌های دیگه با ویدیو embed */
} VS_Source;

typedef struct {
    char     url[VS_MAX_URL];
    char     label[VS_MAX_LABEL];  /* "1080p HD"، "720p"، "Original" */
    int      width, height;
    int      bitrate;              /* kbps — صفر اگه مشخص نیست */
    char     ext[8];               /* ".mp4"، ".mkv" */
    BOOL     is_hls;
    BOOL     is_dash;
} VS_Quality;

typedef struct {
    VS_Source  source;
    char       page_url[VS_MAX_URL];   /* URL صفحه‌ی اصلی */
    char       title[256];             /* عنوان ویدیو */
    char       thumbnail[VS_MAX_URL];  /* URL thumbnail */
    VS_Quality qualities[VS_MAX_QUALITIES];
    int        quality_count;
    int        best_idx;              /* ایندکس بهترین کیفیت */
} VS_VideoInfo;

/* callback که GUO ثبت می‌کنه — وقتی sniffer یه ویدیو پیدا کرد */
typedef void (*VS_FoundCb)(const VS_VideoInfo *info, void *userdata);

/* ── ساخت/نابود ── */
IDM_PluginHooks *vs_plugin_create (VS_FoundCb on_found, void *userdata);
void             vs_plugin_destroy(IDM_PluginHooks *plug);

/* ── تشخیص نوع URL ── */
VS_Source vs_detect_source(const char *url);
BOOL      vs_is_video_url (const char *url);

/* چک واقعی content-type سرور (شبکه‌ای) — برای URL هایی که از
   روی پسوند یا دامنه قابل تشخیص نیستن (VS_SRC_GENERIC) */
BOOL vs_confirm_video_mime(const char *url);

/* چک کردن دامنه‌های استثنا (تبلیغات و...) — برای جلوگیری از
   تشخیص اشتباه ویدیو روی محتوای غیرمرتبط */
BOOL vs_is_excluded_domain(const char *url);

/* ── sniff یه URL ──
   غیر-blocking: نتیجه از طریق callback میاد
   referer: صفحه‌ای که ویدیو توشه (برای authentication) */
void vs_sniff_async(IDM_PluginHooks *plug, const char *url,
                    const char *referer, const char *page_title);

/* ── sniff همزمان (blocking) — برای تست ── */
BOOL vs_sniff_sync(const char *url, const char *referer,
                   VS_VideoInfo *out);

/* ── helper: پیدا کردن بهترین کیفیت ── */
int  vs_best_quality(const VS_VideoInfo *info);

/* ── label ساختن از width/height ── */
void vs_make_label(int w, int h, int bitrate, char *out, int sz);

/* ── helper: runtime callback update ── */
void vs_set_callback(IDM_PluginHooks *plug,
                     VS_FoundCb on_found, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_VIDEO_SNIFFER_H */
