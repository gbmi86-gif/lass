/* plugin_hls.h
   ─────────────────────────────────────────────────────────────
   HLS / M3U8 Download Plugin

   پشتیبانی از:
     - HLS (HTTP Live Streaming)  — .m3u8
     - فقط HTTP — بدون هیچ lib خارجی
     - Multi-quality: انتخاب بالاترین bandwidth
     - Segment دانلود موازی با thread pool ساده
     - خروجی: فایل .ts یا .mp4 (concat ساده)

   استفاده:
     hls_is_m3u8(url)     — تشخیص HLS URL
     hls_fetch_playlist() — دانلود و parse پلی‌لیست
     hls_download()       — دانلود همه segments
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_HLS_H
#define IDM_PLUGIN_HLS_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HLS_MAX_SEGMENTS  4096
#define HLS_MAX_URL       2048
#define HLS_MAX_VARIANTS  32

/* ── یه segment در پلی‌لیست ── */
typedef struct {
    char   url[HLS_MAX_URL];
    double duration;        /* ثانیه */
    int    index;
} HLS_Segment;

/* ── یه variant stream (multi-quality) ── */
typedef struct {
    char   url[HLS_MAX_URL];
    int    bandwidth;       /* bps */
    int    width, height;
    char   codecs[128];
} HLS_Variant;

/* ── نتیجه parse پلی‌لیست ── */
typedef struct {
    /* اگه master playlist بود */
    HLS_Variant variants[HLS_MAX_VARIANTS];
    int         variant_count;
    int         best_variant;  /* ایندکس بالاترین bandwidth */

    /* اگه media playlist بود */
    HLS_Segment segments[HLS_MAX_SEGMENTS];
    int         segment_count;
    double      total_duration; /* ثانیه */
    int         target_duration;
    BOOL        is_endlist;     /* #EXT-X-ENDLIST موجوده؟ */
    BOOL        is_master;      /* master playlist با variants؟ */
    char        base_url[HLS_MAX_URL]; /* برای URL relative */
} HLS_Playlist;

/* ── progress callback ── */
typedef void (*HLS_ProgressCb)(int done, int total, void *userdata);

/* ── API ── */
BOOL       hls_is_m3u8_url  (const char *url);
BOOL       hls_is_m3u8_content(const char *content, int len);

IDM_Result hls_fetch_playlist(const char *url, HLS_Playlist *out);
IDM_Result hls_resolve_best  (HLS_Playlist *pl, HLS_Playlist *media_out);

/* دانلود همه segments و concat به out_path */
IDM_Result hls_download(const char    *playlist_url,
                         const char    *out_path,
                         int            max_threads,  /* 1-8 */
                         HLS_ProgressCb on_progress,
                         void          *userdata);

/* plugin lifecycle */
IDM_PluginHooks *hls_plugin_create(void);
void             hls_plugin_destroy(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_HLS_H */
