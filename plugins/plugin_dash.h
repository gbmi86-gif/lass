/* plugin_dash.h
   ─────────────────────────────────────────────────────────────
   DASH / MPD: تجزیه‌ی manifest و ساخت representation ها.

   پشتیبانی (VOD، یک Period، بدون DRM):
     - SegmentTemplate با duration و $Number$ یا $Number%0Nd$ و $RepresentationID$ و $Bandwidth$
     - SegmentList با Initialization@sourceURL و SegmentURL@media
     - BaseURL در سطح MPD، Period، AdaptationSet و Representation

   رد صریح (دلیل در unsupported یا reject_reason):
     - چند Period، ContentProtection (DRM)، SegmentTimeline، SegmentBase،
       mediaRange/indexRange، WebM، manifest زنده (type="dynamic")

   این ماژول فقط manifest را می‌فهمد؛ دانلود در dash_download.c و mux در mp4_mux.c است.
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_DASH_H
#define LASS_PLUGIN_DASH_H

#include <stddef.h>
#include <stdint.h>
#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_MAX_REPRESENTATIONS  64
#define DASH_MAX_SEGMENTS         8192
#define DASH_MAX_URL              2048
#define DASH_MAX_UNSUPPORTED      160

/* یک representation؛ آدرس‌ها مطلق‌اند */
typedef struct {
    char      id[64];
    int       bandwidth;              /* bps */
    int       width, height;
    char      codecs[128];
    char      mime_type[64];

    char      init_url[DASH_MAX_URL];       /* ۰ طول یعنی ندارد */
    char      media_template[DASH_MAX_URL]; /* فقط برای use_template؛ هنوز $Number$ دارد */
    long long start_number;
    long long timescale;
    long long duration;               /* مدت هر segment در timescale */
    long long segment_count;          /* فقط برای use_template */
    BOOL      use_template;

    char    (*segments)[DASH_MAX_URL];   /* فقط برای SegmentList؛ روی heap */
    int       segment_list_count;
} DASH_Representation;

/* نتیجه‌ی تجزیه‌ی یک MPD */
typedef struct {
    char      base_url[DASH_MAX_URL];
    double    total_duration;           /* ثانیه؛ ۰ یعنی نامشخص */
    BOOL      is_live;
    char      unsupported[DASH_MAX_UNSUPPORTED];   /* دلیل رد کل manifest؛ خالی یعنی قابل دانلود */
    char      reject_reason[DASH_MAX_UNSUPPORTED]; /* دلیل رد representation هایی که پشتیبانی نشدند */

    DASH_Representation video[DASH_MAX_REPRESENTATIONS];
    int                 video_count;
    int                 best_video;     /* بالاترین bandwidth */

    DASH_Representation audio[DASH_MAX_REPRESENTATIONS];
    int                 audio_count;
    int                 best_audio;
} DASH_Manifest;

BOOL       dash_is_mpd_url    (const char *url);
BOOL       dash_is_mpd_content(const char *text, int len);

/* دانلود و تجزیه؛ mf باید صفر باشد. در شکست حافظه‌ی segment ها آزاد می‌شود. */
IDM_Result dash_fetch_manifest(const char *url, DASH_Manifest *mf);
/* تجزیه‌ی متن در حافظه؛ mf باید صفر باشد. FALSE یعنی متن MPD نیست. */
BOOL       dash_parse_manifest(const char *mpd_url, const char *text, size_t len,
                               DASH_Manifest *mf);
/* آزاد کردن آرایه‌های segment؛ بعد از استفاده از mf همیشه صدا بزنید */
void       dash_manifest_free (DASH_Manifest *mf);

/* تعداد segment های یک representation؛ ۰ یعنی نامشخص */
uint32_t   dash_rep_segment_count(const DASH_Representation *r);
/* آدرس مطلق segment با اندیس صفرمبنا؛ FALSE اگر اندیس نامعتبر یا بافر کوچک باشد */
BOOL       dash_rep_segment_url  (const DASH_Representation *r, uint32_t index,
                                  char *out, size_t out_sz);

/* plugin lifecycle */
IDM_PluginHooks *dash_plugin_create (void);
void             dash_plugin_destroy(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_DASH_H */
