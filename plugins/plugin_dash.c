/* plugin_dash.c
   ─────────────────────────────────────────────────────────────
   DASH / MPD Plugin  –  pure WinINet, no external XML lib

   روش parse:
     - MPD یه XML هستش — از strstr برای extract attribute استفاده می‌کنیم
     - نه DOM parser نه SAX — فقط string search برای سادگی
     - برای MPD های پیچیده (namespace، CDATA) کافیه

   مراحل dash_download:
     1. fetch + parse MPD
     2. انتخاب بهترین Representation (بالاترین bandwidth)
     3. دانلود init segment
     4. دانلود media segments به ترتیب
     5. concat همه به out_path
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

#include "plugin_dash.h"
#include "../src/lass_log.h"
#include "../src/types.h"

#define DASH_FETCH_BUF (1024 * 1024)  /* 1MB برای MPD */
#define DASH_READ_BUF  (64  * 1024)

/* ======================================================
   HTTP helper
   ====================================================== */
static char *dash_http_get(const char *url, int *out_len) {
    if (!url || !out_len) return NULL;
    *out_len = 0;

    HINTERNET hInet = InternetOpenA("Lass/DASH",
                                     INTERNET_OPEN_TYPE_PRECONFIG,
                                     NULL, NULL, 0);
    if (!hInet) return NULL;

    DWORD to = 20000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    HINTERNET hUrl = InternetOpenUrlA(hInet, url, NULL, 0,
                                       INTERNET_FLAG_RELOAD |
                                       INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) { InternetCloseHandle(hInet); return NULL; }

    char  *buf  = (char *)malloc(DASH_FETCH_BUF + 1);
    int    total = 0;
    DWORD  read  = 0;
    while (total < DASH_FETCH_BUF) {
        if (!InternetReadFile(hUrl, buf + total,
                              DASH_FETCH_BUF - total, &read) || read == 0)
            break;
        total += (int)read;
    }
    buf[total] = '\0';
    *out_len   = total;

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);
    return buf;
}

/* ======================================================
   XML attribute helpers
   ====================================================== */

/* مقدار attribute رو در XML tag پیدا کن */
static BOOL xml_attr(const char *tag_start, const char *attr_name,
                      char *out, int out_sz) {
    if (!tag_start || !attr_name || !out) return FALSE;
    char search[128];
    snprintf(search, sizeof(search), "%s=\"", attr_name);
    const char *p = strstr(tag_start, search);
    /* فقط داخل همین tag — تا '>' */
    const char *tag_end = strchr(tag_start, '>');
    if (tag_end && p > tag_end) return FALSE;
    if (!p) return FALSE;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return (i > 0);
}

/* مقدار عددی attribute */
static long long xml_attr_ll(const char *tag, const char *attr, long long def) {
    char buf[64] = {0};
    if (!xml_attr(tag, attr, buf, sizeof(buf))) return def;
    return (long long)atoll(buf);
}

/* ======================================================
   URL resolution
   ====================================================== */
static void dash_resolve_url(const char *base, const char *rel,
                               char *out, int out_sz) {
    if (!base || !rel || !out) return;
    if (_strnicmp(rel, "http://", 7) == 0 ||
        _strnicmp(rel, "https://", 8) == 0) {
        strncpy(out, rel, out_sz - 1);
        out[out_sz-1] = '\0';
        return;
    }
    if (rel[0] == '/') {
        const char *se = strstr(base, "://");
        if (!se) { strncpy(out, rel, out_sz-1); return; }
        const char *he = strchr(se + 3, '/');
        int plen = he ? (int)(he - base) : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s", plen, base, rel);
        return;
    }
    const char *last = strrchr(base, '/');
    if (last) {
        int dlen = (int)(last - base) + 1;
        snprintf(out, out_sz, "%.*s%s", dlen, base, rel);
    } else {
        strncpy(out, rel, out_sz - 1);
    }
}

/* ======================================================
   SegmentTemplate URL ساخت:  $Number$ / $Time$ / $RepresentationID$
   ====================================================== */
static void dash_expand_template(const char *tmpl, const char *rep_id,
                                   long long number, long long time_val,
                                   char *out, int out_sz) {
    if (!tmpl || !out) return;
    char tmp[DASH_MAX_URL] = {0};
    strncpy(tmp, tmpl, sizeof(tmp) - 1);

    /* $RepresentationID$ */
    {
        char *p = strstr(tmp, "$RepresentationID$");
        if (p && rep_id) {
            char rest[DASH_MAX_URL];
            strncpy(rest, p + 18, sizeof(rest) - 1);
            *p = '\0';
            snprintf(p, sizeof(tmp) - (p - tmp), "%s%s", rep_id, rest);
        }
    }
    /* $Number$ و $Number%05d$ */
    {
        char *p = strstr(tmp, "$Number");
        if (p) {
            char fmt_part[32] = {0};
            char *end = strchr(p + 7, '$');
            if (end) {
                int flen = (int)(end - p - 7);
                if (flen > 0) strncpy(fmt_part, p + 7, flen < 30 ? flen : 30);
            }
            char num_str[32];
            if (fmt_part[0])
                snprintf(num_str, sizeof(num_str), fmt_part, (int)number);
            else
                snprintf(num_str, sizeof(num_str), "%lld", number);

            char rest[DASH_MAX_URL] = {0};
            if (end) strncpy(rest, end + 1, sizeof(rest) - 1);
            *p = '\0';
            snprintf(p, sizeof(tmp) - (p - tmp), "%s%s", num_str, rest);
        }
    }
    /* $Time$ */
    {
        char *p = strstr(tmp, "$Time$");
        if (p) {
            char ts[32]; snprintf(ts, sizeof(ts), "%lld", time_val);
            char rest[DASH_MAX_URL] = {0};
            strncpy(rest, p + 6, sizeof(rest) - 1);
            *p = '\0';
            snprintf(p, sizeof(tmp) - (p - tmp), "%s%s", ts, rest);
        }
    }
    strncpy(out, tmp, out_sz - 1);
    out[out_sz - 1] = '\0';
}

/* ======================================================
   Parse یه AdaptationSet از XML
   ====================================================== */
static int parse_adaptation_set(const char *xml_section,
                                  const char *base_url,
                                  DASH_Representation *reps,
                                  int max_reps,
                                  const char *seg_template_parent) {
    int count = 0;
    const char *p = xml_section;

    /* SegmentTemplate در AdaptationSet سطح */
    char parent_template[DASH_MAX_URL] = {0};
    char parent_init[DASH_MAX_URL]     = {0};
    long long parent_timescale = 1;
    long long parent_duration  = 0;
    long long parent_start     = 1;
    {
        const char *st = strstr(p, "<SegmentTemplate");
        if (st && strchr(st, '>')) {
            xml_attr(st, "media",       parent_template, sizeof(parent_template));
            xml_attr(st, "initialization", parent_init, sizeof(parent_init));
            parent_timescale = xml_attr_ll(st, "timescale", 1);
            parent_duration  = xml_attr_ll(st, "duration",  0);
            parent_start     = xml_attr_ll(st, "startNumber", 1);
        }
        (void)seg_template_parent;
    }

    while (count < max_reps) {
        const char *rep = strstr(p, "<Representation");
        if (!rep) break;
        const char *rep_end = strstr(rep + 1, "</Representation>");
        if (!rep_end) {
            const char *self_close = strstr(rep, "/>");
            rep_end = self_close ? self_close + 2 : rep + 500;
        }

        DASH_Representation *r = &reps[count];
        memset(r, 0, sizeof(*r));

        xml_attr(rep, "id",        r->id,        sizeof(r->id));
        xml_attr(rep, "codecs",    r->codecs,    sizeof(r->codecs));
        xml_attr(rep, "mimeType",  r->mime_type, sizeof(r->mime_type));
        r->bandwidth = (int)xml_attr_ll(rep, "bandwidth", 0);
        r->width     = (int)xml_attr_ll(rep, "width",     0);
        r->height    = (int)xml_attr_ll(rep, "height",    0);

        /* SegmentTemplate: ابتدا در Representation، سپس parent */
        const char *st = strstr(rep, "<SegmentTemplate");
        if (!st || st > rep_end) {
            /* استفاده از parent template */
            strncpy(r->media_template, parent_template, DASH_MAX_URL - 1);
            strncpy(r->init_url,       parent_init,     DASH_MAX_URL - 1);
            r->timescale    = parent_timescale;
            r->duration     = parent_duration;
            r->start_number = parent_start;
        } else {
            xml_attr(st, "media",          r->media_template, sizeof(r->media_template));
            xml_attr(st, "initialization", r->init_url,       sizeof(r->init_url));
            r->timescale    = xml_attr_ll(st, "timescale",   1);
            r->duration     = xml_attr_ll(st, "duration",    0);
            r->start_number = xml_attr_ll(st, "startNumber", 1);
        }

        /* resolve URLs نسبی */
        if (r->init_url[0] && r->init_url[0] != 'h') {
            char tmp[DASH_MAX_URL];
            dash_resolve_url(base_url, r->init_url, tmp, sizeof(tmp));
            strncpy(r->init_url, tmp, DASH_MAX_URL - 1);
        }
        if (r->media_template[0]) {
            r->use_template = TRUE;
            /* segment count از duration */
            /* بعداً از totalDuration محاسبه میشه */
        }

        /* SegmentList */
        const char *sl = strstr(rep, "<SegmentList");
        if (sl && sl < rep_end) {
            r->use_template = FALSE;
            const char *seg = sl;
            while (r->segment_list_count < DASH_MAX_SEGMENTS) {
                const char *su = strstr(seg, "<SegmentURL");
                if (!su || su > rep_end) break;
                char media_url[DASH_MAX_URL] = {0};
                xml_attr(su, "media", media_url, sizeof(media_url));
                if (media_url[0]) {
                    char resolved[DASH_MAX_URL];
                    dash_resolve_url(base_url, media_url, resolved, sizeof(resolved));
                    strncpy(r->segments[r->segment_list_count],
                            resolved, DASH_MAX_URL - 1);
                    r->segment_list_count++;
                }
                seg = su + 11;
            }
        }

        count++;
        p = rep_end;
    }
    return count;
}

/* ======================================================
   Public: dash_fetch_manifest
   ====================================================== */
IDM_Result dash_fetch_manifest(const char *url, DASH_Manifest *out) {
    if (!url || !out) return IDM_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    snprintf(out->base_url, sizeof(out->base_url), "%s", url);
    char *last_sl = strrchr(out->base_url, '/');
    if (last_sl) *(last_sl + 1) = '\0';

    int len = 0;
    char *xml = dash_http_get(url, &len);
    if (!xml || len < 10) { free(xml); return IDM_ERR_NETWORK; }

    /* بررسی MPD */
    if (!strstr(xml, "<MPD") && !strstr(xml, "MPD")) {
        free(xml); return IDM_ERR_INVALID;
    }

    /* mediaPresentationDuration */
    char dur_str[64] = {0};
    const char *mpd_tag = strstr(xml, "<MPD");
    if (mpd_tag) {
        xml_attr(mpd_tag, "mediaPresentationDuration", dur_str, sizeof(dur_str));
        /* فرمت: PT1H2M3.4S */
        if (dur_str[0]) {
            double h=0,m=0,s=0;
            sscanf(dur_str, "PT%lfH%lfM%lfS", &h, &m, &s);
            if (s == 0 && m == 0 && h == 0) {
                sscanf(dur_str, "PT%lfM%lfS", &m, &s);
                if (s == 0 && m == 0) sscanf(dur_str, "PT%lfS", &s);
            }
            out->total_duration = h * 3600 + m * 60 + s;
        }
        char ptype[32] = {0};
        xml_attr(mpd_tag, "type", ptype, sizeof(ptype));
        out->is_live = (strcmp(ptype, "dynamic") == 0);
    }

    /* BaseURL */
    const char *burl = strstr(xml, "<BaseURL>");
    if (burl) {
        burl += 9;
        const char *burl_end = strstr(burl, "</BaseURL>");
        if (burl_end) {
            int blen = (int)(burl_end - burl);
            if (blen < DASH_MAX_URL) {
                char base[DASH_MAX_URL] = {0};
                memcpy(base, burl, blen);
                dash_resolve_url(url, base, out->base_url, sizeof(out->base_url));
            }
        }
    }

    /* parse AdaptationSets */
    const char *p = xml;
    while (1) {
        const char *aset = strstr(p, "<AdaptationSet");
        if (!aset) break;
        const char *aset_end = strstr(aset + 1, "</AdaptationSet>");
        if (!aset_end) {
            const char *sc = strstr(aset, "/>");
            aset_end = sc ? sc + 2 : aset + 2000;
        }

        /* تشخیص video یا audio از contentType یا mimeType */
        char ctype[64]  = {0};
        char mime[64]   = {0};
        xml_attr(aset, "contentType", ctype, sizeof(ctype));
        xml_attr(aset, "mimeType",    mime,  sizeof(mime));

        BOOL is_video = (strstr(ctype,"video") || strstr(mime,"video"));
        BOOL is_audio = (strstr(ctype,"audio") || strstr(mime,"audio"));

        if (!is_video && !is_audio) {
            /* حدس از اولین Representation */
            const char *first_rep = strstr(aset, "<Representation");
            if (first_rep) {
                char rep_mime[64]={0};
                xml_attr(first_rep,"mimeType",rep_mime,sizeof(rep_mime));
                is_video = (strstr(rep_mime,"video") != NULL);
                is_audio = (strstr(rep_mime,"audio") != NULL);
                if (!is_video && !is_audio) is_video = TRUE; /* پیش‌فرض */
            }
        }

        /* extract section تا aset_end */
        int section_len = (int)(aset_end - aset);
        char *section   = (char *)malloc(section_len + 1);
        if (section) {
            memcpy(section, aset, section_len);
            section[section_len] = '\0';

            if (is_video && out->video_count < DASH_MAX_REPRESENTATIONS) {
                int added = parse_adaptation_set(section, out->base_url,
                                                  out->video + out->video_count,
                                                  DASH_MAX_REPRESENTATIONS - out->video_count,
                                                  NULL);
                out->video_count += added;
            } else if (is_audio && out->audio_count < DASH_MAX_REPRESENTATIONS) {
                int added = parse_adaptation_set(section, out->base_url,
                                                  out->audio + out->audio_count,
                                                  DASH_MAX_REPRESENTATIONS - out->audio_count,
                                                  NULL);
                out->audio_count += added;
            }
            free(section);
        }

        p = aset_end;
    }

    free(xml);

    /* انتخاب بهترین Representation */
    out->best_video = 0;
    for (int i = 1; i < out->video_count; i++)
        if (out->video[i].bandwidth > out->video[out->best_video].bandwidth)
            out->best_video = i;

    out->best_audio = 0;
    for (int i = 1; i < out->audio_count; i++)
        if (out->audio[i].bandwidth > out->audio[out->best_audio].bandwidth)
            out->best_audio = i;

    /* محاسبه segment_count از total_duration */
    for (int i = 0; i < out->video_count; i++) {
        DASH_Representation *r = &out->video[i];
        if (r->use_template && r->duration > 0 && r->timescale > 0
            && out->total_duration > 0) {
            double seg_dur = (double)r->duration / r->timescale;
            r->segment_count = (long long)ceil(out->total_duration / seg_dur);
        }
    }

    LOGI(LOG_MOD_DASH, "Manifest parsed: video=%d audio=%d dur=%.1fs live=%d",
         out->video_count, out->audio_count,
         out->total_duration, out->is_live);

    if (out->video_count == 0 && out->audio_count == 0)
        return IDM_ERR_INVALID;

    return IDM_OK;
}

/* ======================================================
   دانلود یه segment و append به فایل
   ====================================================== */
static BOOL dash_download_segment(const char *seg_url, HANDLE hOut,
                                   int retry_count) {
    if (!seg_url || !seg_url[0]) return FALSE;

    for (int attempt = 0; attempt <= retry_count; attempt++) {
        HINTERNET hInet = InternetOpenA("Lass/DASH-seg",
                                         INTERNET_OPEN_TYPE_PRECONFIG,
                                         NULL, NULL, 0);
        if (!hInet) continue;

        DWORD to = 30000;
        InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

        HINTERNET hUrl = InternetOpenUrlA(hInet, seg_url, NULL, 0,
                                           INTERNET_FLAG_RELOAD |
                                           INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hUrl) { InternetCloseHandle(hInet); Sleep(300); continue; }

        char   buf[DASH_READ_BUF];
        DWORD  read = 0;
        BOOL   ok   = TRUE;

        while (1) {
            if (!InternetReadFile(hUrl, buf, sizeof(buf), &read) || read == 0)
                break;
            DWORD written = 0;
            if (!WriteFile(hOut, buf, read, &written, NULL) || written != read) {
                ok = FALSE; break;
            }
        }

        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInet);

        if (ok) return TRUE;
        Sleep(300);
    }
    return FALSE;
}

/* ======================================================
   Public: dash_download
   ====================================================== */
IDM_Result dash_download(const char    *mpd_url,
                          const char    *out_path,
                          DASH_ProgressCb on_progress,
                          void           *userdata) {
    if (!mpd_url || !out_path) return IDM_ERR_INVALID;

    /* ── ۱. parse manifest — روی heap ── */
    DASH_Manifest *mf = (DASH_Manifest *)calloc(1, sizeof(DASH_Manifest));
    if (!mf) return IDM_ERR_INVALID;

    IDM_Result r = dash_fetch_manifest(mpd_url, mf);
    if (r != IDM_OK) { free(mf); return r; }

    /* ── ۲. انتخاب representation ── */
    DASH_Representation *vid = (mf->video_count > 0)
                               ? &mf->video[mf->best_video] : NULL;
    DASH_Representation *aud = (mf->audio_count > 0)
                               ? &mf->audio[mf->best_audio] : NULL;

    if (!vid && !aud) { free(mf); return IDM_ERR_INVALID; }

    LOGI(LOG_MOD_DASH, "Best video: %dx%d @ %d bps",
         vid ? vid->width : 0, vid ? vid->height : 0,
         vid ? vid->bandwidth : 0);

    /* ── ۳. باز کردن فایل خروجی ── */
    HANDLE hOut = CreateFileA(out_path,
                               GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL |
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hOut == INVALID_HANDLE_VALUE) { free(mf); return IDM_ERR_IO; }

    int done  = 0;
    /* تعداد کل: init + segments برای video + audio */
    long long total_segs = 0;
    if (vid) total_segs += (vid->use_template ? vid->segment_count
                                               : vid->segment_list_count) + 1;
    if (aud) total_segs += (aud->use_template ? aud->segment_count
                                               : aud->segment_list_count) + 1;

    /* ── تابع helper برای دانلود یه representation ── */
    DASH_Representation *reps[2] = {vid, aud};
    for (int ri = 0; ri < 2; ri++) {
        DASH_Representation *rep = reps[ri];
        if (!rep) continue;

        /* init segment */
        if (rep->init_url[0]) {
            if (!dash_download_segment(rep->init_url, hOut, 2)) {
                LOGW(LOG_MOD_DASH, "Failed init segment: %s", rep->init_url);
                CloseHandle(hOut); free(mf); return IDM_ERR_NETWORK;
            }
            done++;
            if (on_progress) on_progress((int)done, (int)total_segs, userdata);
        }

        if (rep->use_template) {
            /* SegmentTemplate mode */
            long long seg_count = rep->segment_count;
            if (seg_count <= 0) seg_count = 100; /* fallback */

            long long time_val = 0;
            for (long long i = 0; i < seg_count; i++) {
                char seg_url[DASH_MAX_URL];
                dash_expand_template(rep->media_template, rep->id,
                                      rep->start_number + i,
                                      time_val, seg_url, sizeof(seg_url));

                char resolved[DASH_MAX_URL];
                if (seg_url[0] != 'h')
                    dash_resolve_url(mf->base_url, seg_url, resolved, sizeof(resolved));
                else
                    strncpy(resolved, seg_url, DASH_MAX_URL - 1);

                if (!dash_download_segment(resolved, hOut, 2)) {
                    LOGW(LOG_MOD_DASH, "Failed segment #%lld", i);
                    /* ادامه بده — ممکنه stream تموم شده باشه */
                    break;
                }

                time_val += rep->duration;
                done++;
                if (on_progress)
                    on_progress((int)done, (int)total_segs, userdata);

                if (done % 10 == 0)
                    LOGI(LOG_MOD_DASH, "Progress: %d/%lld segments",
                         done, total_segs);
            }
        } else {
            /* SegmentList mode */
            for (int i = 0; i < rep->segment_list_count; i++) {
                if (!rep->segments[i][0]) continue;
                if (!dash_download_segment(rep->segments[i], hOut, 2)) {
                    LOGW(LOG_MOD_DASH, "Failed segment %d", i);
                    break;
                }
                done++;
                if (on_progress)
                    on_progress((int)done, (int)total_segs, userdata);
            }
        }
    }

    CloseHandle(hOut);
    free(mf);

    LOGI(LOG_MOD_DASH, "Complete: %d segments downloaded to %s",
         done, out_path);
    return IDM_OK;
}

/* ======================================================
   تشخیص MPD
   ====================================================== */
BOOL dash_is_mpd_url(const char *url) {
    if (!url) return FALSE;
    const char *q = strchr(url, '?');
    int base_len = q ? (int)(q - url) : (int)strlen(url);
    if (base_len >= 4 &&
        _strnicmp(url + base_len - 4, ".mpd", 4) == 0)
        return TRUE;
    if (q && strstr(q, "mpd")) return TRUE;
    return FALSE;
}

BOOL dash_is_mpd_content(const char *text, int len) {
    if (!text || len < 4) return FALSE;
    return (strstr(text, "<MPD") != NULL);
}

/* ======================================================
   Plugin lifecycle
   ====================================================== */
static int  dash_on_init   (void) { return 0; }
static void dash_on_destroy(void) {}

IDM_PluginHooks *dash_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    plug->name        = "dash_mpd";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = dash_on_init;
    plug->on_destroy  = dash_on_destroy;
    LOGI(LOG_MOD_DASH, "plugin created");
    return plug;
}

void dash_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    free(plug);
}
