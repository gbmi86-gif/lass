/* plugin_hls.c
   ─────────────────────────────────────────────────────────────
   HLS / M3U8: تشخیص URL، parse کامل playlist و پلاگین.
   بدون lib خارجی (فقط WinINet برای sniffer).

   دانلود کامل جریان در src/hls_download.c است؛ این فایل فقط
   playlist را می‌فهمد و نتیجه را گزارش می‌دهد. parse دیگر از
   strtok استفاده نمی‌کند، پس امن برای چند thread است.
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

#include "plugin_hls.h"
#include "../src/lass_log.h"
#include "../src/types.h"

#define HLS_FETCH_BUF  (512 * 1024)   /* سقف متن playlist در مسیر sniffer */

/* ======================================================
   HTTP helper ساده برای متن (فقط sniffer)
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

    HINTERNET hUrl = InternetOpenUrlA(hInet, url, NULL, 0,
                                      INTERNET_FLAG_RELOAD |
                                      INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) { InternetCloseHandle(hInet); return NULL; }

    char  *buf   = (char *)malloc(HLS_FETCH_BUF + 1);
    int    total = 0;
    DWORD  read  = 0;
    if (!buf) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInet);
        return NULL;
    }
    while (total < HLS_FETCH_BUF) {
        if (!InternetReadFile(hUrl, buf + total,
                              (DWORD)(HLS_FETCH_BUF - total), &read) || read == 0)
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
   حل آدرس نسبی به مطلق
   ====================================================== */
static void hls_resolve_url(const char *base, const char *rel,
                            char *out, int out_sz) {
    if (!base || !rel || !out) return;

    if (_strnicmp(rel, "http://", 7) == 0 || _strnicmp(rel, "https://", 8) == 0) {
        snprintf(out, out_sz, "%s", rel);
        return;
    }

    if (rel[0] == '/') {
        /* از ریشه‌ی دامنه */
        const char *scheme_end = strstr(base, "://");
        if (!scheme_end) { snprintf(out, out_sz, "%s", rel); return; }
        const char *host_end = strchr(scheme_end + 3, '/');
        int prefix_len = host_end ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s", prefix_len, base, rel);
        return;
    }

    /* نسبی: پوشه‌ی base */
    const char *last_slash = strrchr(base, '/');
    if (last_slash) {
        int dir_len = (int)(last_slash - base) + 1;
        snprintf(out, out_sz, "%.*s%s", dir_len, base, rel);
    } else {
        snprintf(out, out_sz, "%s", rel);
    }
}

/* ======================================================
   کمک‌های parse
   ====================================================== */
static int hls_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* IV به شکل 0x و ۳۲ رقم هگز */
static BOOL hls_parse_iv(const char *s, uint8_t out[16]) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (strlen(s) != 32) return FALSE;
    for (int i = 0; i < 16; i++) {
        int hi = hls_hexval(s[2 * i]);
        int lo = hls_hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return FALSE;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return TRUE;
}

/* مقدار یک attribute از خط تگ؛ مقدار داخل کوتیشن هم خوانده می‌شود */
static BOOL hls_attr(const char *attrs, const char *name, char *out, size_t out_sz) {
    size_t nl = strlen(name);
    const char *p = attrs;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *kbeg = p;
        while (*p && *p != '=' && *p != ',') p++;
        size_t kl = (size_t)(p - kbeg);
        if (*p != '=') continue;        /* attribute بدون مقدار */
        p++;                            /* از '=' رد شو */
        const char *vbeg;
        size_t vl;
        if (*p == '"') {
            p++;
            vbeg = p;
            while (*p && *p != '"') p++;
            vl = (size_t)(p - vbeg);
            if (*p == '"') p++;
        } else {
            vbeg = p;
            while (*p && *p != ',') p++;
            vl = (size_t)(p - vbeg);
        }
        if (kl == nl && _strnicmp(kbeg, name, nl) == 0) {
            if (vl >= out_sz) return FALSE;
            memcpy(out, vbeg, vl);
            out[vl] = '\0';
            return TRUE;
        }
    }
    return FALSE;
}

/* EXT-X-BYTERANGE:<n>[@<o>] */
static BOOL hls_parse_byterange(const char *v, uint64_t *len, uint64_t *off, BOOL *has_off) {
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    if (end == v || n == 0) return FALSE;
    *len = (uint64_t)n;
    *off = 0;
    *has_off = FALSE;
    if (*end == '@') {
        char *end2 = NULL;
        unsigned long long o = strtoull(end + 1, &end2, 10);
        if (end2 == end + 1) return FALSE;
        *off = (uint64_t)o;
        *has_off = TRUE;
    }
    return TRUE;
}

/* EXT-X-KEY: METHOD=NONE یعنی رمزنشده؛ AES-128 با کلید؛ بقیه ناپشتیبانی‌شده */
static IDM_Result hls_handle_key(const char *url, const char *attrs,
                                 HLS_Playlist *out, int *cur_key) {
    char method[32] = "", uri[HLS_MAX_URL] = "", iv_s[64] = "", fmt[64] = "";
    hls_attr(attrs, "METHOD", method, sizeof(method));
    if (method[0] == '\0' || _stricmp(method, "NONE") == 0) {
        *cur_key = -1;
        return IDM_OK;
    }
    if (_stricmp(method, "AES-128") != 0) {
        snprintf(out->unsupported, sizeof(out->unsupported),
                 "encryption method %s is not supported", method);
        *cur_key = -1;
        return IDM_OK;
    }
    hls_attr(attrs, "KEYFORMAT", fmt, sizeof(fmt));
    if (fmt[0] && _stricmp(fmt, "identity") != 0) {
        snprintf(out->unsupported, sizeof(out->unsupported),
                 "key format %s is not supported (DRM)", fmt);
        *cur_key = -1;
        return IDM_OK;
    }
    if (!hls_attr(attrs, "URI", uri, sizeof(uri)) || uri[0] == '\0')
        return IDM_ERR_INVALID;
    hls_attr(attrs, "IV", iv_s, sizeof(iv_s));

    HLS_Key k;
    memset(&k, 0, sizeof(k));
    char abs_uri[HLS_MAX_URL];
    hls_resolve_url(url, uri, abs_uri, sizeof(abs_uri));
    snprintf(k.uri, sizeof(k.uri), "%s", abs_uri);
    if (iv_s[0]) {
        if (!hls_parse_iv(iv_s, k.iv)) return IDM_ERR_INVALID;
        k.has_iv = TRUE;
    }

    for (int i = 0; i < out->key_count; i++) {
        const HLS_Key *e = &out->keys[i];
        if (strcmp(e->uri, k.uri) == 0 && e->has_iv == k.has_iv &&
            (!k.has_iv || memcmp(e->iv, k.iv, sizeof(k.iv)) == 0)) {
            *cur_key = i;
            return IDM_OK;
        }
    }
    if (out->key_count >= HLS_MAX_KEYS) {
        snprintf(out->unsupported, sizeof(out->unsupported), "too many encryption keys");
        *cur_key = -1;
        return IDM_OK;
    }
    out->keys[out->key_count] = k;
    *cur_key = out->key_count++;
    return IDM_OK;
}

/* EXT-X-MAP: بخش init؛ با BYTERANGE پشتیبانی نمی‌شود */
static IDM_Result hls_handle_map(const char *url, const char *attrs,
                                 HLS_Playlist *out, int *cur_map) {
    char uri[HLS_MAX_URL] = "", br[32] = "";
    if (!hls_attr(attrs, "URI", uri, sizeof(uri)) || uri[0] == '\0')
        return IDM_ERR_INVALID;
    if (hls_attr(attrs, "BYTERANGE", br, sizeof(br))) {
        snprintf(out->unsupported, sizeof(out->unsupported),
                 "init section with a byte range is not supported");
        *cur_map = -1;
        return IDM_OK;
    }
    char abs_uri[HLS_MAX_URL];
    hls_resolve_url(url, uri, abs_uri, sizeof(abs_uri));
    for (int i = 0; i < out->map_count; i++) {
        if (strcmp(out->maps[i].uri, abs_uri) == 0) {
            *cur_map = i;
            return IDM_OK;
        }
    }
    if (out->map_count >= HLS_MAX_MAPS) {
        snprintf(out->unsupported, sizeof(out->unsupported), "too many init sections");
        *cur_map = -1;
        return IDM_OK;
    }
    snprintf(out->maps[out->map_count].uri, sizeof(out->maps[out->map_count].uri),
             "%s", abs_uri);
    *cur_map = out->map_count++;
    return IDM_OK;
}

/* ======================================================
   parse کامل playlist (master یا media)
   ====================================================== */
IDM_Result hls_parse_text(const char *url, const char *text, int len, HLS_Playlist *out) {
    if (!url || !text || !out || len < 7) return IDM_ERR_INVALID;
    if (strncmp(text, "#EXTM3U", 7) != 0) return IDM_ERR_INVALID;

    snprintf(out->base_url, sizeof(out->base_url), "%s", url);
    char *slash = strrchr(out->base_url, '/');
    if (slash) *(slash + 1) = '\0';

    int         cur_key        = -1;
    int         cur_map        = -1;
    BOOL        pend_variant   = FALSE;
    HLS_Variant pv;
    memset(&pv, 0, sizeof(pv));
    double      pend_dur       = 0.0;
    BOOL        have_dur       = FALSE;
    BOOL        pend_range     = FALSE;
    BOOL        pend_range_off = FALSE;
    uint64_t    pend_range_len = 0;
    uint64_t    pend_range_off_v = 0;
    char        last_range_uri[HLS_MAX_URL] = "";
    uint64_t    last_range_end = 0;

    const char *end = text + len;
    const char *p = text;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *ls = p;
        const char *le = nl ? nl : end;
        p = nl ? nl + 1 : end;
        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
        size_t ll = (size_t)(le - ls);
        if (ll == 0) continue;
        if (ll >= HLS_MAX_URL) return IDM_ERR_INVALID;

        char line[HLS_MAX_URL];
        memcpy(line, ls, ll);
        line[ll] = '\0';

        if (line[0] == '#') {
            if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
                const char *a = line + 18;
                char buf[64];
                memset(&pv, 0, sizeof(pv));
                if (hls_attr(a, "BANDWIDTH", buf, sizeof(buf))) pv.bandwidth = atoi(buf);
                if (hls_attr(a, "RESOLUTION", buf, sizeof(buf)))
                    sscanf(buf, "%dx%d", &pv.width, &pv.height);
                hls_attr(a, "CODECS", pv.codecs, sizeof(pv.codecs));
                hls_attr(a, "AUDIO", pv.audio_group, sizeof(pv.audio_group));
                pend_variant = TRUE;
                out->is_master = TRUE;
            } else if (strncmp(line, "#EXT-X-MEDIA:", 13) == 0) {
                char type[32], uri[HLS_MAX_URL];
                if (hls_attr(line + 13, "TYPE", type, sizeof(type)) &&
                    _stricmp(type, "AUDIO") == 0 &&
                    hls_attr(line + 13, "URI", uri, sizeof(uri)))
                    out->has_separate_audio = TRUE;
            } else if (strncmp(line, "#EXTINF:", 8) == 0) {
                pend_dur = strtod(line + 8, NULL);
                have_dur = TRUE;
            } else if (strncmp(line, "#EXT-X-BYTERANGE:", 17) == 0) {
                uint64_t blen = 0, boff = 0;
                BOOL has_off = FALSE;
                if (!hls_parse_byterange(line + 17, &blen, &boff, &has_off))
                    return IDM_ERR_INVALID;
                pend_range = TRUE;
                pend_range_len = blen;
                pend_range_off = has_off;
                pend_range_off_v = boff;
            } else if (strncmp(line, "#EXT-X-KEY:", 11) == 0) {
                IDM_Result kr = hls_handle_key(url, line + 11, out, &cur_key);
                if (kr != IDM_OK) return kr;
            } else if (strncmp(line, "#EXT-X-MAP:", 11) == 0) {
                IDM_Result mr = hls_handle_map(url, line + 11, out, &cur_map);
                if (mr != IDM_OK) return mr;
            } else if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                out->media_sequence = (uint64_t)strtoull(line + 22, NULL, 10);
            } else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
                out->target_duration = atoi(line + 22);
            } else if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
                out->is_endlist = TRUE;
            }
            continue;
        }

        /* خط URI: variant یا segment */
        char abs_uri[HLS_MAX_URL];
        hls_resolve_url(url, line, abs_uri, sizeof(abs_uri));

        if (pend_variant) {
            if (out->variant_count >= HLS_MAX_VARIANTS) {
                snprintf(out->unsupported, sizeof(out->unsupported),
                         "too many quality variants");
                return IDM_OK;
            }
            snprintf(pv.url, sizeof(pv.url), "%s", abs_uri);
            out->variants[out->variant_count++] = pv;
            pend_variant = FALSE;
            continue;
        }

        if (out->segment_count >= HLS_MAX_SEGMENTS) {
            snprintf(out->unsupported, sizeof(out->unsupported),
                     "playlist has more than %d segments", HLS_MAX_SEGMENTS);
            return IDM_OK;
        }
        HLS_Segment *s = &out->segments[out->segment_count];
        memset(s, 0, sizeof(*s));
        snprintf(s->url, sizeof(s->url), "%s", abs_uri);
        s->duration  = have_dur ? pend_dur : 0.0;
        s->index     = out->segment_count;
        s->key_index = cur_key;
        s->map_index = cur_map;
        s->media_seq = out->media_sequence + (uint64_t)out->segment_count;
        if (pend_range) {
            uint64_t off = pend_range_off ? pend_range_off_v
                         : (strcmp(last_range_uri, s->url) == 0 ? last_range_end : 0u);
            s->has_range = TRUE;
            s->range_off = off;
            s->range_len = pend_range_len;
            snprintf(last_range_uri, sizeof(last_range_uri), "%s", s->url);
            last_range_end = off + pend_range_len;
        }
        out->total_duration += s->duration;
        out->segment_count++;
        have_dur = FALSE;
        pend_range = FALSE;
        pend_range_off = FALSE;
    }

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

    LOGI(LOG_MOD_HLS, "Parsed: %s | segments=%d variants=%d keys=%d maps=%d endlist=%d",
         out->is_master ? "master" : "media", out->segment_count,
         out->variant_count, out->key_count, out->map_count, (int)out->is_endlist);
    return IDM_OK;
}

/* ======================================================
   دانلود متن playlist برای sniffer (بدون هدر و کوکی دانلود)
   ====================================================== */
IDM_Result hls_fetch_playlist(const char *url, HLS_Playlist *out) {
    if (!url || !out) return IDM_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    int text_len = 0;
    char *text = hls_http_get_text(url, &text_len);
    if (!text || text_len < 7) {
        free(text);
        return IDM_ERR_NETWORK;
    }
    IDM_Result r = hls_parse_text(url, text, text_len, out);
    free(text);
    return r;
}

/* ======================================================
   تشخیص HLS URL و محتوا
   ====================================================== */
BOOL hls_is_m3u8_url(const char *url) {
    if (!url) return FALSE;
    const char *q = strchr(url, '?');
    int base_len = q ? (int)(q - url) : (int)strlen(url);

    /* پسوند .m3u8 */
    if (base_len >= 5 && _strnicmp(url + base_len - 5, ".m3u8", 5) == 0)
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
