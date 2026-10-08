/* plugin_dash.h
   ─────────────────────────────────────────────────────────────
   DASH (Dynamic Adaptive Streaming over HTTP) Plugin

   پشتیبانی از:
     - MPEG-DASH  .mpd  (ISO Base Media File Format)
     - AdaptationSet انتخاب بهترین کیفیت (bandwidth-based)
     - SegmentTemplate + SegmentList + SegmentBase
     - Multi-period (ساده)

   بدون lib خارجی — parse XML با WinAPI + دانلود با WinINet
   خروجی: فایل .mp4 یا .ts (concat segments)
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_DASH_H
#define LASS_PLUGIN_DASH_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_MAX_REPRESENTATIONS  64
#define DASH_MAX_SEGMENTS        8192
#define DASH_MAX_URL             2048

/* ── یه Representation (یه کیفیت از یه stream) ── */
typedef struct {
    char     id[64];
    int      bandwidth;       /* bps */
    int      width, height;
    char     codecs[128];
    char     mime_type[64];   /* video/mp4 یا audio/mp4 */

    /* segment info */
    char     init_url[DASH_MAX_URL];    /* Initialization segment */
    char     media_template[DASH_MAX_URL]; /* $Number$ یا $Time$ */
    long long start_number;
    long long segment_count;
    long long timescale;
    long long duration;       /* per segment در timescale units */

    /* SegmentList mode */
    char     segments[DASH_MAX_SEGMENTS][DASH_MAX_URL];
    int      segment_list_count;

    BOOL     use_template;    /* TRUE=SegmentTemplate, FALSE=SegmentList */
} DASH_Representation;

/* ── نتیجه parse MPD ── */
typedef struct {
    char     base_url[DASH_MAX_URL];
    double   total_duration;   /* ثانیه */
    BOOL     is_live;          /* live stream */

    DASH_Representation video[DASH_MAX_REPRESENTATIONS];
    int                  video_count;
    int                  best_video;   /* ایندکس بالاترین bandwidth */

    DASH_Representation audio[DASH_MAX_REPRESENTATIONS];
    int                  audio_count;
    int                  best_audio;
} DASH_Manifest;

/* progress callback */
typedef void (*DASH_ProgressCb)(int done, int total, void *userdata);

/* ── API ── */
BOOL       dash_is_mpd_url    (const char *url);
BOOL       dash_is_mpd_content(const char *text, int len);

IDM_Result dash_fetch_manifest(const char *url, DASH_Manifest *out);

/* دانلود بهترین video+audio و mux (concat) به out_path */
IDM_Result dash_download(const char    *mpd_url,
                          const char    *out_path,
                          DASH_ProgressCb on_progress,
                          void           *userdata);

/* plugin lifecycle */
IDM_PluginHooks *dash_plugin_create (void);
void             dash_plugin_destroy(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_DASH_H */
