/* plugin_hls.h
   ─────────────────────────────────────────────────────────────
   HLS / M3U8: تشخیص و parse پلی‌لیست‌ها

   این فایل فقط تشخیص و parse را پوشش می‌دهد. دانلود کامل جریان
   (گرفتن segment ها، رمزگشایی AES-128، ادامه‌ی دانلود در کانتینر
   .tels) در src/hls_download.c است.

   آنچه parse می‌شود:
     - master و media playlist (بهترین variant = بالاترین bandwidth)
     - EXT-X-KEY با METHOD=AES-128 (IV صریح یا پیش‌فرض شماره‌ی توالی)
     - EXT-X-MAP (بخش init مثل fMP4)
     - EXT-X-BYTERANGE (زیربازه‌ای از یک منبع)
     - EXT-X-MEDIA-SEQUENCE، EXT-X-TARGETDURATION، EXT-X-ENDLIST

   آنچه پشتیبانی نمی‌شود و در HLS_Playlist.unsupported گزارش می‌شود:
     - METHOD غیر از NONE و AES-128 (مثل SAMPLE-AES)
     - KEYFORMAT غیر از identity (DRM)
     - صدای جدا (EXT-X-MEDIA TYPE=AUDIO با URI) برای variant انتخاب‌شده
     - EXT-X-MAP با BYTERANGE
     - بیش از HLS_MAX_SEGMENTS segment
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_HLS_H
#define IDM_PLUGIN_HLS_H

#include <windows.h>
#include <stdint.h>
#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HLS_MAX_SEGMENTS  8192
#define HLS_MAX_URL       2048
#define HLS_MAX_VARIANTS  32
#define HLS_MAX_KEYS      32
#define HLS_MAX_MAPS      16
#define HLS_MAX_UNSUPP    128

/* ── یک variant در master playlist ── */
typedef struct {
    char   url[HLS_MAX_URL];
    int    bandwidth;           /* bps */
    int    width, height;
    char   codecs[128];
    char   audio_group[64];     /* AUDIO="..."؛ خالی یعنی صدای جدا ندارد */
} HLS_Variant;

/* ── کلید AES-128 ── */
typedef struct {
    char    uri[HLS_MAX_URL];   /* آدرس کامل کلید */
    BOOL    has_iv;             /* IV صریح در EXT-X-KEY بود؟ */
    uint8_t iv[16];
} HLS_Key;

/* ── بخش init (EXT-X-MAP) ── */
typedef struct {
    char uri[HLS_MAX_URL];      /* آدرس کامل بخش init */
} HLS_Map;

/* ── یک segment در media playlist ── */
typedef struct {
    char     url[HLS_MAX_URL];
    double   duration;          /* ثانیه */
    int      index;
    int      key_index;         /* -1 یعنی رمزنشده */
    int      map_index;         /* -1 یعنی بدون EXT-X-MAP */
    BOOL     has_range;         /* EXT-X-BYTERANGE */
    uint64_t range_off;         /* آفست در منبع */
    uint64_t range_len;         /* طول زیربازه */
    uint64_t media_seq;         /* شماره‌ی توالی رسانه (برای IV پیش‌فرض) */
} HLS_Segment;

/* ── نتیجه‌ی parse ── */
typedef struct {
    HLS_Variant variants[HLS_MAX_VARIANTS];
    int         variant_count;
    int         best_variant;           /* بالاترین bandwidth */
    BOOL        has_separate_audio;     /* EXT-X-MEDIA TYPE=AUDIO با URI */

    HLS_Segment segments[HLS_MAX_SEGMENTS];
    int         segment_count;
    double      total_duration;         /* ثانیه */
    int         target_duration;
    BOOL        is_endlist;             /* #EXT-X-ENDLIST موجود است؟ */
    BOOL        is_master;              /* master playlist با variants؟ */
    char        base_url[HLS_MAX_URL];
    uint64_t    media_sequence;         /* #EXT-X-MEDIA-SEQUENCE */

    HLS_Key     keys[HLS_MAX_KEYS];
    int         key_count;
    HLS_Map     maps[HLS_MAX_MAPS];
    int         map_count;

    /* "" یعنی همه‌ی چیزهای دیده‌شده پشتیبانی می‌شوند؛ وگرنه دلیل قابل‌فهم */
    char        unsupported[HLS_MAX_UNSUPP];
} HLS_Playlist;

/* ── API ── */
BOOL       hls_is_m3u8_url    (const char *url);
BOOL       hls_is_m3u8_content(const char *content, int len);

/* متن playlist را parse می‌کند؛ متن ورودی تغییر نمی‌کند. url برای حل آدرس‌های نسبی است.
   out باید صفر باشد (با calloc ساخته شده). */
IDM_Result hls_parse_text(const char *url, const char *text, int len, HLS_Playlist *out);

/* دانلود ساده‌ی playlist بدون هدر و کوکی‌های دانلود (برای تشخیص در sniffer) */
IDM_Result hls_fetch_playlist(const char *url, HLS_Playlist *out);

/* plugin lifecycle */
IDM_PluginHooks *hls_plugin_create(void);
void             hls_plugin_destroy(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_HLS_H */
