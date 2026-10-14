/* plugin_hls.c
   ─────────────────────────────────────────────────────────────
   HLS / M3U8 Download Plugin  –  pure WinINet, no lib

   مراحل:
     1. hls_fetch_playlist: دانلود و parse m3u8
     2. اگه master بود: hls_resolve_best بهترین variant رو انتخاب کنه
     3. hls_download: هر segment رو دانلود کنه و concat کنه

   فرمت m3u8 (ساده):
     #EXTM3U
     #EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=720x480
     http://...720p.m3u8
     ...
     یا:
     #EXTINF:9.009,
     http://media.example.com/first.ts
     ...
     #EXT-X-ENDLIST
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "plugin_hls.h"
#include "../src/lass_log.h"
#include "../src/types.h"

#define HLS_FETCH_BUF  (512 * 1024)  /* 512KB برای دانلود playlist */
#define HLS_SEG_BUF    (64  * 1024)  /* 64KB read buffer */

/* ======================================================
   HTTP helper ساده برای دانلود متن
   ====================================================== */
static char *hls_http_get_text(const char *url, int *out_len) {
    if (!url || !out_len) return NULL;
    *out_len = 0;

    HINTERNET hInet = InternetOpenA("Lass/HLS",
                                     INTERNET_OPEN_TYPE_PRECONFIG,
                                     NULL, NULL, 0);
    if (!hInet) return NULL;

    DWORD to = 20000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    HINTERNET hUrl = InternetOpenUrlA(hInet, url,
                                       NULL, 0,
                                       INTERNET_FLAG_RELOAD |
                                       INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) { InternetCloseHandle(hInet); return NULL; }

    char  *buf  = (char *)malloc(HLS_FETCH_BUF + 1);
    int    total = 0;
    DWORD  read  = 0;

    while (total < HLS_FETCH_BUF) {
        if (!InternetReadFile(hUrl, buf + total,
                              HLS_FETCH_BUF - total, &read) || read == 0)
            break;
        total += (int)read;
    }
    buf[total] = '\0';
    *out_len = total;

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);
    return buf;
}

/* ======================================================
   URL resolution: relative → absolute
   ====================================================== */
static void hls_resolve_url(const char *base, const char *rel,
                              char *out, int out_sz) {
    if (!base || !rel || !out) return;

    if (_strnicmp(rel, "http://", 7) == 0 ||
        _strnicmp(rel, "https://", 8) == 0) {
        /* مطلق */
        snprintf(out, out_sz, "%s", rel);
        return;
    }

    if (rel[0] == '/') {
        /* از ریشه دامنه */
        const char *scheme_end = strstr(base, "://");
        if (!scheme_end) { snprintf(out, out_sz, "%s", rel); return; }
        const char *host_end = strchr(scheme_end + 3, '/');
        int prefix_len = host_end ? (int)(host_end - base)
                                  : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s", prefix_len, base, rel);
        return;
    }

    /* نسبی: پیدا کردن directory base */
    const char *last_slash = strrchr(base, '/');
    if (last_slash) {
        int dir_len = (int)(last_slash - base) + 1;
        snprintf(out, out_sz, "%.*s%s", dir_len, base, rel);
    } else {
        snprintf(out, out_sz, "%s", rel);
    }
}

/* ======================================================
   Parse M3U8
   ====================================================== */
static void hls_parse_stream_inf(const char *line, HLS_Variant *v) {
    /* BANDWIDTH= */
    const char *bw = strstr(line, "BANDWIDTH=");
    if (bw) v->bandwidth = atoi(bw + 10);

    /* RESOLUTION= */
    const char *res = strstr(line, "RESOLUTION=");
    if (res) sscanf(res + 11, "%dx%d", &v->width, &v->height);

    /* CODECS= */
    const char *cod = strstr(line, "CODECS=\"");
    if (cod) {
        cod += 8;
        const char *end = strchr(cod, '"');
        if (end) {
            int len = (int)(end - cod);
            if (len >= (int)sizeof(v->codecs)) len = (int)sizeof(v->codecs) - 1;
            memcpy(v->codecs, cod, len);
            v->codecs[len] = '\0';
        }
    }
}

IDM_Result hls_fetch_playlist(const char *url, HLS_Playlist *out) {
    if (!url || !out) return IDM_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    /* base URL */
    snprintf(out->base_url, sizeof(out->base_url), "%s", url);
    char *last_slash = strrchr(out->base_url, '/');
    if (last_slash) *(last_slash + 1) = '\0';

    int   text_len = 0;
    char *text = hls_http_get_text(url, &text_len);
    if (!text || text_len < 7) {
        free(text);
        return IDM_ERR_NETWORK;
    }

    /* بررسی اینکه واقعاً m3u8 هست */
    if (strncmp(text, "#EXTM3U", 7) != 0) {
        free(text);
        return IDM_ERR_INVALID;
    }

    /* parse خط‌به‌خط */
    char *line = strtok(text, "\n");
    HLS_Variant pending_variant = {0};
    BOOL has_pending_variant = FALSE;

    while (line) {
        /* trim \r */
        int ll = (int)strlen(line);
        while (ll > 0 && (line[ll-1] == '\r' || line[ll-1] == ' '))
            line[--ll] = '\0';

        if (ll == 0) { line = strtok(NULL, "\n"); continue; }

        if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            /* master playlist variant */
            out->is_master = TRUE;
            memset(&pending_variant, 0, sizeof(pending_variant));
            hls_parse_stream_inf(line + 18, &pending_variant);
            has_pending_variant = TRUE;

        } else if (strncmp(line, "#EXTINF:", 8) == 0) {
            /* media segment */
            if (out->segment_count < HLS_MAX_SEGMENTS) {
                double dur = atof(line + 8);
                out->segments[out->segment_count].duration = dur;
                out->segments[out->segment_count].index    = out->segment_count;
                out->total_duration += dur;
            }

        } else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
            out->target_duration = atoi(line + 22);

        } else if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
            out->is_endlist = TRUE;

        } else if (line[0] != '#') {
            /* URL (variant یا segment) */
            char resolved[HLS_MAX_URL];
            hls_resolve_url(url, line, resolved, sizeof(resolved));

            if (has_pending_variant && out->variant_count < HLS_MAX_VARIANTS) {
                snprintf(pending_variant.url, sizeof(pending_variant.url),
                         "%s", resolved);
                out->variants[out->variant_count++] = pending_variant;
                has_pending_variant = FALSE;
            } else if (out->segment_count < HLS_MAX_SEGMENTS) {
                snprintf(out->segments[out->segment_count].url,
                         sizeof(out->segments[0].url), "%s", resolved);
                out->segment_count++;
            }
        }

        line = strtok(NULL, "\n");
    }

    free(text);

    /* بهترین variant = بالاترین bandwidth */
    if (out->variant_count > 0) {
        int best_bw = -1;
        for (int i = 0; i < out->variant_count; i++) {
            if (out->variants[i].bandwidth > best_bw) {
                best_bw = out->variants[i].bandwidth;
                out->best_variant = i;
            }
        }
    }

    LOGI(LOG_MOD_HLS, "Parsed: %s | segments=%d variants=%d duration=%.1fs",
         out->is_master ? "master" : "media",
         out->segment_count, out->variant_count, out->total_duration);

    return IDM_OK;
}

IDM_Result hls_resolve_best(HLS_Playlist *pl, HLS_Playlist *media_out) {
    if (!pl || !media_out) return IDM_ERR_INVALID;
    if (!pl->is_master || pl->variant_count == 0) {
        /* از قبل media playlist بود */
        *media_out = *pl;
        return IDM_OK;
    }
    const char *best_url = pl->variants[pl->best_variant].url;
    LOGI(LOG_MOD_HLS, "Best variant: %d bps  %dx%d  %s",
         pl->variants[pl->best_variant].bandwidth,
         pl->variants[pl->best_variant].width,
         pl->variants[pl->best_variant].height,
         best_url);
    return hls_fetch_playlist(best_url, media_out);
}

/* ======================================================
   hls_download  –  دانلود + concat همه segments
   ====================================================== */
IDM_Result hls_download(const char    *playlist_url,
                         const char    *out_path,
                         int            max_threads,
                         HLS_ProgressCb on_progress,
                         void          *userdata) {
    if (!playlist_url || !out_path) return IDM_ERR_INVALID;
    if (max_threads < 1) max_threads = 1;
    if (max_threads > 8) max_threads = 8;

    /* ── ۱. fetch playlist — روی heap (هر HLS_Playlist ~8MB روی stack!) ── */
    HLS_Playlist *pl = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    HLS_Playlist *media_pl = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    if (!pl || !media_pl) { free(pl); free(media_pl); return IDM_ERR_INVALID; }

    IDM_Result r = hls_fetch_playlist(playlist_url, pl);
    if (r != IDM_OK) { free(pl); free(media_pl); return r; }

    r = hls_resolve_best(pl, media_pl);
    if (r != IDM_OK) { free(pl); free(media_pl); return r; }

    if (media_pl->segment_count == 0) { free(pl); free(media_pl); return IDM_ERR_INVALID; }

    LOGI(LOG_MOD_HLS, "Downloading %d segments to %s",
         media_pl->segment_count, out_path);

    /* ── ۲. باز کردن فایل خروجی ── */
    HANDLE hOut = CreateFileA(out_path,
                               GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL |
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hOut == INVALID_HANDLE_VALUE) { free(pl); free(media_pl); return IDM_ERR_IO; }

    int done = 0;

    for (int i = 0; i < media_pl->segment_count; i++) {
        const char *seg_url = media_pl->segments[i].url;
        if (!seg_url[0]) continue;

        int   text_len = 0;
        char *data = hls_http_get_text(seg_url, &text_len);

        if (!data || text_len == 0) {
            LOGW(LOG_MOD_HLS, "Failed segment #%d: %s", i, seg_url);
            free(data);
            Sleep(500);
            data = hls_http_get_text(seg_url, &text_len);
        }

        if (data && text_len > 0) {
            DWORD written = 0;
            WriteFile(hOut, data, (DWORD)text_len, &written, NULL);
            free(data);
        } else {
            free(data);
            CloseHandle(hOut);
            free(pl); free(media_pl);
            return IDM_ERR_NETWORK;
        }

        done++;
        if (on_progress)
            on_progress(done, media_pl->segment_count, userdata);

        if (done % 10 == 0)
            LOGI(LOG_MOD_HLS, "Progress: %d/%d segments",
                 done, media_pl->segment_count);
    }

    CloseHandle(hOut);
    LOGI(LOG_MOD_HLS, "Complete: %d segments, %.1f seconds",
         media_pl->segment_count, media_pl->total_duration);

    free(pl);
    free(media_pl);
    return IDM_OK;
}

/* ======================================================
   تشخیص HLS URL
   ====================================================== */
BOOL hls_is_m3u8_url(const char *url) {
    if (!url) return FALSE;
    const char *q = strchr(url, '?');
    int base_len = q ? (int)(q - url) : (int)strlen(url);

    /* پسوند .m3u8 */
    if (base_len >= 5 &&
        _strnicmp(url + base_len - 5, ".m3u8", 5) == 0)
        return TRUE;

    /* یا پارامتر type=m3u8 در query */
    if (q && strstr(q, "m3u8")) return TRUE;

    return FALSE;
}

BOOL hls_is_m3u8_content(const char *content, int len) {
    if (!content || len < 7) return FALSE;
    return (strncmp(content, "#EXTM3U", 7) == 0);
}

/* ======================================================
   Plugin hooks
   ====================================================== */
static int  hls_on_init   (void) { return 0; }
static void hls_on_destroy(void) {}

IDM_PluginHooks *hls_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    plug->name        = "hls_m3u8";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = hls_on_init;
    plug->on_destroy  = hls_on_destroy;
    LOGI(LOG_MOD_HLS, "plugin created");
    return plug;
}

void hls_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    free(plug);
}
