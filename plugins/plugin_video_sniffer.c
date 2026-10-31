/* plugin_video_sniffer.c
   ─────────────────────────────────────────────────────────────
   Lass – Video Sniffer Plugin

   روش کار:
     1. URL رو دریافت می‌کنه (از browser_ext یا clipboard)
     2. تشخیص میده از کدوم سایته
     3. اگه لینک مستقیم ویدیو هست → یه quality entry می‌سازه
     4. اگه HLS/DASH هست → به plugin_hls/dash واگذار می‌کنه
     5. اگه YouTube/آپارات/... هست → API مناسب رو صدا می‌زنه
     6. نتیجه رو از طریق callback به GUO می‌فرسته
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "security_fixes.h"
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "plugin_video_sniffer.h"
#include "plugin_hls.h"
#include "plugin_dash.h"
#include "../src/lass_log.h"
#include "../src/shutdown_brake.h"

/* ── پسوندهای ویدیو مستقیم ── */
/* حداقل سایز برای اینکه یه فایل عمومی (بدون پسوند/سایت مشخص)
   به‌عنوان ویدیوی واقعی در نظر گرفته بشه، نه thumbnail/preview/
   pre-roll تبلیغاتی کوچیک. مقدار از تحلیل افزونه‌های مشابه. */
#define VS_GENERIC_MIN_VIDEO_SIZE (5 * 1024 * 1024) /* 5MB */

static const char *VIDEO_EXTS[] = {
    ".mp4",".mkv",".avi",".mov",".wmv",".flv",".webm",
    ".m4v",".3gp",".ts",".mpeg",".mpg",".ogv",NULL
};

/* ── mime types ویدیو ── */
static const char *VIDEO_MIMES[] = {
    "video/mp4","video/x-matroska","video/webm","video/x-msvideo",
    "video/quicktime","video/x-ms-wmv","video/x-flv","video/mp2t",
    "application/vnd.apple.mpegurl","application/dash+xml",NULL
};

/* بررسی اینکه یه content-type مربوط به ویدیو هست یا نه — مفید
   برای لینک‌های CDN توکن‌دار که از روی URL نمیشه تشخیص داد ولی
   با چک کردن header سرور میشه فهمید ویدیو هست. در حال حاضر
   توسط vs_confirm_video_mime برای doublecheck استفاده میشه. */
static BOOL vs_is_video_mime(const char *content_type) {
    if (!content_type || !content_type[0]) return FALSE;
    char clean[128] = {0};
    safe_strncpy(clean, content_type, sizeof(clean));
    char *semi = strchr(clean, ';');
    if (semi) *semi = '\0';
    for (int i = 0; VIDEO_MIMES[i]; i++)
        if (_stricmp(clean, VIDEO_MIMES[i]) == 0) return TRUE;
    return FALSE;
}

/* چک واقعی content-type سرور — برای URL هایی که از روی پسوند/سایت
   قابل تشخیص نیستن (یعنی VS_SRC_GENERIC). این تابع شبکه استفاده
   می‌کنه پس فقط جایی که UI صریحاً نیاز داره صدا زده میشه، نه در
   مسیر سریع vs_detect_source. */
BOOL vs_confirm_video_mime(const char *url) {
    if (!url) return FALSE;
    if (vs_is_excluded_domain(url)) return FALSE;

    HINTERNET hi = InternetOpenA("Lass/VideoSniffer",
                                  INTERNET_OPEN_TYPE_PRECONFIG,
                                  NULL, NULL, 0);
    if (!hi) return FALSE;

    DWORD to = 8000;
    InternetSetOptionA(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));

    HINTERNET hu = InternetOpenUrlA(hi, url, NULL, 0,
                                     INTERNET_FLAG_RELOAD |
                                     INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hu) { InternetCloseHandle(hi); return FALSE; }

    char ct[128] = {0};
    DWORD ct_sz = sizeof(ct);
    BOOL is_video = FALSE;
    if (HttpQueryInfoA(hu, HTTP_QUERY_CONTENT_TYPE, ct, &ct_sz, NULL))
        is_video = vs_is_video_mime(ct);

    /* فیلتر سایز: یه فایل ویدیویی واقعی معمولاً چندین مگابایته.
       اگه خیلی کوچیکه (مثلاً thumbnail یا پیش‌نمایش چندثانیه‌ای
       یا تبلیغ pre-roll)، احتمالاً چیزی نیست که کاربر واقعاً
       بخواد به‌عنوان "دانلود ویدیو" ببینه. */
    if (is_video) {
        char cl[32] = {0};
        DWORD cl_sz = sizeof(cl);
        if (HttpQueryInfoA(hu, HTTP_QUERY_CONTENT_LENGTH, cl, &cl_sz, NULL)) {
            uint64_t content_len = _strtoui64(cl, NULL, 10);
            if (content_len > 0 && content_len < VS_GENERIC_MIN_VIDEO_SIZE)
                is_video = FALSE;
        }
    }

    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    return is_video;
}

/* ======================================================
   ساختار داخلی
   ====================================================== */
typedef struct {
    VS_FoundCb   on_found;
    void        *userdata;
    volatile LONG active_workers; /* تعداد vs_async_worker در حال اجرا —
                                      برای جلوگیری از use-after-free در
                                      destroy، همون الگویی که در
                                      browser_ext پیدا و رفع شد */
    CRITICAL_SECTION lock;
} VS_Priv;

/* ── async worker context ── */
typedef struct {
    VS_Priv *priv;
    char     url[VS_MAX_URL];
    char     referer[VS_MAX_URL];
    char     title[256];
} VS_WorkCtx;

/* ======================================================
   HTTP helper — fetch متن کوچک (برای API response)
   ====================================================== */
static char *vs_http_get(const char *url, const char *referer,
                          int *out_len) {
    if (!url || !out_len) return NULL;
    *out_len = 0;

    HINTERNET hi = InternetOpenA("Lass/VideoSniffer",
                                  INTERNET_OPEN_TYPE_PRECONFIG,
                                  NULL, NULL, 0);
    if (!hi) return NULL;

    DWORD to = 15000;
    InternetSetOptionA(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    /* اضافه کردن Referer header اگه داریم */
    char headers[512] = {0};
    if (referer && referer[0])
        snprintf(headers, sizeof(headers), "Referer: %s\r\n", referer);

    HINTERNET hu = InternetOpenUrlA(hi, url,
                                     headers[0] ? headers : NULL,
                                     headers[0] ? (DWORD)-1 : 0,
                                     INTERNET_FLAG_RELOAD |
                                     INTERNET_FLAG_NO_CACHE_WRITE |
                                     INTERNET_FLAG_SECURE |
                                     INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                     INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
    if (!hu) { InternetCloseHandle(hi); return NULL; }

    /* بررسی Content-Type — فقط نوع متنی رو دانلود می‌کنیم */
    char ct[128] = {0};
    DWORD ct_sz = sizeof(ct);
    HttpQueryInfoA(hu, HTTP_QUERY_CONTENT_TYPE, ct, &ct_sz, NULL);

    /* ۲MB حداکثر برای API response */
    char *buf = (char *)malloc(2*1024*1024 + 1);
    if (!buf) {
        InternetCloseHandle(hu);
        InternetCloseHandle(hi);
        return NULL;
    }
    int total = 0;
    DWORD read_bytes = 0;
    while (total < 2*1024*1024) {
        if (!InternetReadFile(hu, buf+total,
                              2*1024*1024-total, &read_bytes)
            || read_bytes == 0) break;
        total += (int)read_bytes;
    }
    buf[total] = '\0';
    *out_len = total;

    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    return buf;
}

/* ======================================================
   تشخیص نوع URL
   ====================================================== */
/* ── دامنه‌هایی که نباید به‌عنوان ویدیوی قابل‌دانلود تشخیص داده
   بشن — حتی اگه پسوند/mime-type ویدیویی داشته باشن. این الگو از
   بررسی افزونه‌های دانلود منیجر محبوب گرفته شده: بعضی سایت‌ها
   (مثل سرویس‌های استریم DRM-محافظت‌شده یا CDN های خاص) فایل‌های
   ویدیویی موقت/قطعه‌قطعه می‌فرستن که دانلود مستقیمشون یا کار
   نمی‌کنه یا فایل خراب میده. */
static const char *VS_EXCLUDED_DOMAINS[] = {
    "doubleclick.net",      /* تبلیغات */
    "googlesyndication.com",
    "googleadservices.com",
    NULL
};

BOOL vs_is_excluded_domain(const char *url) {
    if (!url) return FALSE;
    for (int i = 0; VS_EXCLUDED_DOMAINS[i]; i++)
        if (strstr(url, VS_EXCLUDED_DOMAINS[i])) return TRUE;
    return FALSE;
}

VS_Source vs_detect_source(const char *url) {
    if (!url) return VS_SRC_UNKNOWN;
    if (vs_is_excluded_domain(url)) return VS_SRC_UNKNOWN;

    if (_strnicmp(url,"rtmp://",7)==0 || _strnicmp(url,"rtmps://",8)==0)
        return VS_SRC_UNKNOWN; /* RTMP از plugin_rtmp handle میشه */

    if (strstr(url,"youtube.com/watch") || strstr(url,"youtu.be/"))
        return VS_SRC_YOUTUBE;
    if (strstr(url,"aparat.com/v/") || strstr(url,"aparat.com/video/"))
        return VS_SRC_APARAT;
    if (strstr(url,"instagram.com/p/") || strstr(url,"instagram.com/reel/"))
        return VS_SRC_INSTAGRAM;
    if (strstr(url,"twitter.com/") || strstr(url,"x.com/"))
        return VS_SRC_TWITTER;
    if (strstr(url,"vimeo.com/"))
        return VS_SRC_VIMEO;

    /* HLS/DASH */
    if (hls_is_m3u8_url(url)) return VS_SRC_HLS;
    if (dash_is_mpd_url(url)) return VS_SRC_DASH;

    /* لینک مستقیم ویدیو */
    char low[VS_MAX_URL];
    strncpy(low, url, sizeof(low)-1);
    CharLowerA(low);
    char *q = strchr(low,'?'); if(q)*q='\0';
    for (int i = 0; VIDEO_EXTS[i]; i++)
        if (strstr(low, VIDEO_EXTS[i])) return VS_SRC_DIRECT;

    return VS_SRC_GENERIC;
}

BOOL vs_is_video_url(const char *url) {
    VS_Source s = vs_detect_source(url);
    return (s != VS_SRC_UNKNOWN && s != VS_SRC_GENERIC);
}

/* ======================================================
   Label ساز
   ====================================================== */
void vs_make_label(int w, int h, int bitrate, char *out, int sz) {
    if (!out || sz <= 0) return;
    (void)w; /* فعلاً فقط height برای label کافیه؛ w برای آینده نگه داشته شده */
    if (h <= 0) {
        if (bitrate > 0)
            snprintf(out, sz, "%d kbps", bitrate);
        else
            snprintf(out, sz, "Original");
        return;
    }
    const char *suffix = (h >= 720) ? " HD" : "";
    if (bitrate > 0)
        snprintf(out, sz, "%dp%s (%d kbps)", h, suffix, bitrate);
    else
        snprintf(out, sz, "%dp%s", h, suffix);
}

/* ======================================================
   ─────────────────────────────────────────────────────
   Sniff functions per platform
   ─────────────────────────────────────────────────────
   ====================================================== */

/* ── لینک مستقیم ── */
static BOOL sniff_direct(const char *url, VS_VideoInfo *out) {
    out->quality_count = 1;
    out->best_idx      = 0;
    VS_Quality *q = &out->qualities[0];
    strncpy(q->url, url, VS_MAX_URL-1);
    strncpy(q->label, "Original", VS_MAX_LABEL-1);

    /* پسوند از URL */
    char low[VS_MAX_URL];
    strncpy(low, url, sizeof(low)-1);
    CharLowerA(low);
    char *p = strchr(low,'?'); if(p)*p='\0';
    const char *dot = strrchr(low, '.');
    if (dot) strncpy(q->ext, dot, sizeof(q->ext)-1);
    else strcpy(q->ext, ".mp4");

    return TRUE;
}

/* ── HLS ── */
static BOOL sniff_hls(const char *url, VS_VideoInfo *out) {
    HLS_Playlist *pl = (HLS_Playlist *)calloc(1, sizeof(*pl));
    if (!pl) return FALSE;

    if (hls_fetch_playlist(url, pl) != IDM_OK) { free(pl); return FALSE; }

    if (pl->is_master && pl->variant_count > 0) {
        /* master playlist — هر variant یه کیفیت هست */
        int cnt = pl->variant_count < VS_MAX_QUALITIES
                  ? pl->variant_count : VS_MAX_QUALITIES;
        for (int i = 0; i < cnt; i++) {
            VS_Quality *q = &out->qualities[i];
            strncpy(q->url, pl->variants[i].url, VS_MAX_URL-1);
            q->width   = pl->variants[i].width;
            q->height  = pl->variants[i].height;
            q->bitrate = pl->variants[i].bandwidth / 1000;
            q->is_hls  = TRUE;
            strcpy(q->ext, ".ts");
            vs_make_label(q->width, q->height, q->bitrate,
                          q->label, VS_MAX_LABEL);
        }
        out->quality_count = cnt;
        out->best_idx      = pl->best_variant < cnt ? pl->best_variant : 0;
    } else {
        /* media playlist مستقیم */
        VS_Quality *q = &out->qualities[0];
        strncpy(q->url, url, VS_MAX_URL-1);
        strncpy(q->label, "HLS Stream", VS_MAX_LABEL-1);
        q->is_hls = TRUE;
        strcpy(q->ext, ".ts");
        out->quality_count = 1;
        out->best_idx      = 0;
    }

    free(pl);
    return (out->quality_count > 0);
}

/* ── DASH ── */
static BOOL sniff_dash(const char *url, VS_VideoInfo *out) {
    DASH_Manifest *mf = (DASH_Manifest *)calloc(1, sizeof(*mf));
    if (!mf) return FALSE;

    if (dash_fetch_manifest(url, mf) != IDM_OK) { free(mf); return FALSE; }

    int cnt = mf->video_count < VS_MAX_QUALITIES
              ? mf->video_count : VS_MAX_QUALITIES;
    for (int i = 0; i < cnt; i++) {
        VS_Quality *q = &out->qualities[i];
        DASH_Representation *r = &mf->video[i];
        strncpy(q->url, url, VS_MAX_URL-1); /* MPD URL — همه کیفیت‌ها */
        q->width   = r->width;
        q->height  = r->height;
        q->bitrate = r->bandwidth / 1000;
        q->is_dash = TRUE;
        strcpy(q->ext, ".mp4");
        vs_make_label(q->width, q->height, q->bitrate,
                      q->label, VS_MAX_LABEL);
    }
    out->quality_count = cnt ? cnt : 1;
    if (cnt == 0) {
        strncpy(out->qualities[0].url,   url,   VS_MAX_URL-1);
        strncpy(out->qualities[0].label, "DASH",VS_MAX_LABEL-1);
        out->qualities[0].is_dash = TRUE;
    }
    out->best_idx = mf->best_video < cnt ? mf->best_video : 0;

    free(mf);
    return TRUE;
}

/* ── آپارات ──
   API: https://www.aparat.com/etc/api/video/videohash/{hash}/one  */
static BOOL sniff_aparat(const char *url, VS_VideoInfo *out) {
    /* استخراج hash از URL: aparat.com/v/XXXXXX */
    const char *hash = NULL;
    const char *p = strstr(url, "/v/");
    if (!p) p = strstr(url, "/video/");
    if (p) {
        p = strchr(p+1, '/');
        if (p) hash = p+1;
    }
    if (!hash || !hash[0]) return FALSE;

    /* trim query/fragment */
    char hash_clean[64] = {0};
    strncpy(hash_clean, hash, sizeof(hash_clean)-1);
    char *end = strpbrk(hash_clean, "/?#");
    if (end) *end = '\0';

    char api[512];
    snprintf(api, sizeof(api),
             "https://www.aparat.com/etc/api/video/videohash/%s/one",
             hash_clean);

    int len = 0;
    char *resp = vs_http_get(api, url, &len);
    if (!resp || len < 10) { free(resp); return FALSE; }

    /* parse JSON — دنبال "file_link" یا "urls" */
    int cnt = 0;
    const char *search = resp;
    while (cnt < VS_MAX_QUALITIES) {
        const char *flink = strstr(search, "\"file_link_all\"");
        if (!flink) flink = strstr(search, "\"file_link\"");
        if (!flink) break;

        const char *colon = strchr(flink, ':');
        if (!colon) break;
        const char *quote = strchr(colon, '"');
        if (!quote) break;
        quote++; /* بعد از quote */

        char video_url[VS_MAX_URL] = {0};
        int ui = 0;
        const char *c = quote;
        while (*c && *c != '"' && ui < VS_MAX_URL-1) {
            if (*c == '\\' && *(c+1)) { c++; } /* escape */
            video_url[ui++] = *c++;
        }
        video_url[ui] = '\0';

        if (video_url[0] && strstr(video_url, "http")) {
            VS_Quality *q = &out->qualities[cnt];
            safe_strncpy(q->url, video_url, VS_MAX_URL);
            snprintf(q->label, VS_MAX_LABEL, "Quality %d", cnt+1);
            strcpy(q->ext, ".mp4");
            cnt++;
        }
        search = colon + 1;
        if (search >= resp + len) break;
    }

    free(resp);
    out->quality_count = cnt;
    out->best_idx      = cnt > 0 ? cnt-1 : 0; /* آخری معمولاً بهتره */
    return (cnt > 0);
}

/* ── YouTube — از yt-dlp process اگه موجود بود ── */
static BOOL sniff_youtube(const char *url, VS_VideoInfo *out) {
    /* راه‌های مختلف:
       ۱. yt-dlp --dump-json -- [url]  اگه نصب بود
       ۲. Invidious API (invidious.io/api/v1/videos/...)
       ۳. نمایش پیام راهنما به کاربر */

    /* اول yt-dlp رو پیدا کنیم */
    char ytdlp[MAX_PATH] = {0};
    DWORD path_len = GetEnvironmentVariableA("PATH", NULL, 0);
    if (path_len > 0 && path_len < 32768) {
        char *paths = (char*)calloc(path_len+2, 1);
        if (paths) {
            GetEnvironmentVariableA("PATH", paths, path_len+1);
            char *seg_start = paths, *p = paths;
            while (*p) {
                if (*p == ';' || !*p) {
                    size_t slen = (size_t)(p - seg_start);
                    if (slen > 0 && slen + 12 < MAX_PATH) {
                        char full[MAX_PATH] = {0};
                        memcpy(full, seg_start, slen);
                        full[slen] = '\\';
                        memcpy(full+slen+1, "yt-dlp.exe", 10);
                        if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) {
                            safe_strncpy(ytdlp, full, MAX_PATH);
                            break;
                        }
                    }
                    if (!*p) break;
                    seg_start = p+1;
                }
                p++;
            }
            free(paths);
        }
    }

    if (ytdlp[0]) {
        /* حیاتی: url از منابع untrusted میاد (می‌تونه از صفحه‌ی
           یه سایت مخرب scrape شده باشه، نه فقط تایپ مستقیم
           کاربر). این url مستقیم داخل command line string
           می‌ره که به CreateProcessA داده میشه. اگه url شامل
           کاراکتر " باشه، می‌تونه از داخل quote خارج بشه و
           command injection بکنه (مثلاً با & یا | که
           cmd.exe/shell بعدی تفسیرشون می‌کنه).
           راه‌حل: رد کردن هر URL ای که کاراکتر " داره — یه URL
           واقعی و معتبر هیچ‌وقت quote خام (بدون escape) نداره،
           پس این یه سیگنال قطعی حمله‌ست. */
        BOOL url_has_quote = FALSE;
        for (const char *p = url; *p; p++)
            if (*p == '"') { url_has_quote = TRUE; break; }

        if (url_has_quote) {
            LOGE(LOG_MOD_SNIFFER,
                 "Rejected yt-dlp invocation: URL contains quote character "
                 "(potential command injection attempt)");
            goto no_ytdlp;
        }

        char cmd[VS_MAX_URL + MAX_PATH + 64];
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -j --no-playlist \"%s\"", ytdlp, url);

        SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
        HANDLE hRead, hWrite;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) goto no_ytdlp;
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {0};
        si.cb          = sizeof(si);
        si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput  = hWrite;
        si.hStdError   = hWrite;

        PROCESS_INFORMATION pi = {0};
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(hRead); CloseHandle(hWrite);
            goto no_ytdlp;
        }
        CloseHandle(hWrite);

        char *json = (char*)calloc(1024*1024, 1);
        int total = 0;
        DWORD r = 0;
        while (total < 1024*1024-1) {
            if (!ReadFile(hRead, json+total, 1024*1024-1-total, &r, NULL)
                || r == 0) break;
            total += (int)r;
        }
        json[total] = '\0';
        CloseHandle(hRead);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        /* parse formats از JSON */
        int cnt = 0;
        const char *s = json;
        while (cnt < VS_MAX_QUALITIES) {
            const char *fmt = strstr(s, "\"url\":");
            if (!fmt) break;
            const char *q1 = strchr(fmt+6, '"');
            if (!q1) break;
            q1++;
            char vu[VS_MAX_URL]={0}; int ui=0;
            const char *c=q1;
            while(*c&&*c!='"'&&ui<VS_MAX_URL-1){
                if(*c=='\\'&&*(c+1)){c++;}
                vu[ui++]=*c++;
            }
            vu[ui]='\0';

            if (vu[0] && strstr(vu,"http") && !strstr(vu,"manifest")) {
                VS_Quality *qq = &out->qualities[cnt];
                safe_strncpy(qq->url, vu, VS_MAX_URL);
                /* height رو هم بگیر */
                const char *hp = strstr(s, "\"height\":");
                if (hp && hp < fmt + 500) {
                    qq->height = atoi(hp+9);
                }
                const char *bp = strstr(s, "\"tbr\":");
                if (bp && bp < fmt + 500) qq->bitrate=(int)atof(bp+6);
                strcpy(qq->ext, ".mp4");
                vs_make_label(0, qq->height, qq->bitrate,
                              qq->label, VS_MAX_LABEL);
                cnt++;
            }
            s = fmt + 6;
        }
        free(json);
        if (cnt > 0) {
            out->quality_count = cnt;
            out->best_idx = cnt-1;
            return TRUE;
        }
    }

no_ytdlp:
    /* yt-dlp نیست — یه entry با راهنما */
    out->quality_count = 1;
    VS_Quality *q = &out->qualities[0];
    strncpy(q->url, url, VS_MAX_URL-1);
    strncpy(q->label, "YouTube (نیاز به yt-dlp)", VS_MAX_LABEL-1);
    strcpy(q->ext, ".mp4");
    out->best_idx = 0;
    return TRUE;
}

/* ── اینستاگرام / توییتر — از JSON embed ── */
static BOOL sniff_instagram(const char *url, VS_VideoInfo *out) {
    /* Instagram: لینک‌های مستقیم معمولاً از /p/SHORTCODE/ میان
       روش: fetch صفحه و پیدا کردن video_url در JSON */
    int len = 0;
    char *html = vs_http_get(url, NULL, &len);
    if (!html || len < 100) { free(html); return FALSE; }

    BOOL found = FALSE;
    const char *s = html;
    int cnt = 0;
    while (cnt < VS_MAX_QUALITIES) {
        const char *vurl = strstr(s, "\"video_url\":");
        if (!vurl) break;
        const char *q1 = strchr(vurl+12, '"');
        if (!q1) break;
        q1++;
        char vu[VS_MAX_URL]={0}; int ui=0;
        const char *c=q1;
        while(*c&&*c!='"'&&ui<VS_MAX_URL-1){
            if(*c=='\\'&&*(c+1)){c++;}
            vu[ui++]=*c++;
        }
        vu[ui]='\0';
        if (vu[0] && strstr(vu,"http")) {
            VS_Quality *q = &out->qualities[cnt];
            safe_strncpy(q->url, vu, VS_MAX_URL);
            snprintf(q->label, VS_MAX_LABEL, "Video %d", cnt+1);
            strcpy(q->ext, ".mp4");
            cnt++;
            found = TRUE;
        }
        s = vurl + 12;
    }
    free(html);
    if (found) { out->quality_count = cnt; out->best_idx = 0; }
    return found;
}

/* ======================================================
   اصلی: sniff_sync
   ====================================================== */
BOOL vs_sniff_sync(const char *url, const char *referer,
                   VS_VideoInfo *out) {
    if (!url || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    strncpy(out->page_url, referer ? referer : url, VS_MAX_URL-1);
    out->source = vs_detect_source(url);

    switch (out->source) {
    case VS_SRC_DIRECT:   return sniff_direct(url, out);
    case VS_SRC_HLS:      return sniff_hls(url, out);
    case VS_SRC_DASH:     return sniff_dash(url, out);
    case VS_SRC_YOUTUBE:  return sniff_youtube(url, out);
    case VS_SRC_APARAT:   return sniff_aparat(url, out);
    case VS_SRC_INSTAGRAM:return sniff_instagram(url, out);
    case VS_SRC_TWITTER:
        /* مشابه اینستاگرام — video_url در JSON */
        return sniff_instagram(url, out);
    case VS_SRC_VIMEO: {
        /* Vimeo config API */
        char api[VS_MAX_URL];
        const char *slash = strrchr(url, '/');
        if (slash) {
            snprintf(api, sizeof(api),
                     "https://player.vimeo.com/video/%s/config", slash+1);
            int len=0;
            char *resp = vs_http_get(api, url, &len);
            if (resp && len > 10) {
                /* دنبال "progressive" urls */
                const char *s = resp; int cnt=0;
                while(cnt < VS_MAX_QUALITIES) {
                    const char *pu = strstr(s, "\"url\":\"http");
                    if (!pu) break;
                    pu += 7;
                    char vu[VS_MAX_URL]={0}; int ui=0;
                    const char *c=pu;
                    while(*c&&*c!='"'&&ui<VS_MAX_URL-1){
                        if(*c=='\\'&&*(c+1)){c++;}
                        vu[ui++]=*c++;
                    }
                    vu[ui]='\0';
                    if (vu[0]) {
                        VS_Quality *q=&out->qualities[cnt];
                        safe_strncpy(q->url, vu, VS_MAX_URL);
                        const char *qp=strstr(s,"\"quality\":");
                        if(qp&&qp<pu+200){
                            char ql[32]={0};
                            const char *qq=strchr(qp,'"'); if(qq){qq++;
                            int qi=0;while(*qq&&*qq!='"'&&qi<31)ql[qi++]=*qq++;
                            ql[qi]='\0'; strncpy(q->label,ql,VS_MAX_LABEL-1);}
                        }
                        strcpy(q->ext,".mp4");
                        cnt++;
                    }
                    s=pu+1;
                }
                free(resp);
                if (cnt>0){ out->quality_count=cnt; out->best_idx=cnt-1; return TRUE;}
            }
            if(resp) free(resp);
        }
        return FALSE;
    }
    default: return FALSE;
    }
}

/* ======================================================
   Async worker
   ====================================================== */
static DWORD WINAPI vs_async_worker(LPVOID param) {
    VS_WorkCtx *ctx = (VS_WorkCtx *)param;
    VS_Priv    *p   = ctx->priv;
    int brake_id = shutdown_brake_register("video_sniffer_worker");

    /* توجه: active_workers از قبل در vs_sniff_async افزایش پیدا
       کرده، قبل از CreateThread — اینجا فقط در پایان decrement
       می‌کنیم */

    VS_VideoInfo *info = (VS_VideoInfo *)calloc(1, sizeof(*info));
    if (info) {
        safe_strncpy(info->title, ctx->title, sizeof(info->title));
        if (vs_sniff_sync(ctx->url, ctx->referer, info)) {
            if (p->on_found) p->on_found(info, p->userdata);
        }
        free(info);
    }
    free(ctx);

    InterlockedDecrement(&p->active_workers);
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ======================================================
   Plugin lifecycle
   ====================================================== */
int  vs_on_init   (void) { return 0; }
void vs_on_destroy(void) {}

IDM_PluginHooks *vs_plugin_create(VS_FoundCb on_found, void *userdata) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    VS_Priv *p = (VS_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->on_found = on_found;
    p->userdata = userdata;

    plug->priv        = p;
    plug->name        = "video_sniffer";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = vs_on_init;
    plug->on_destroy  = vs_on_destroy;

    LOGI("video_sniffer", "plugin created");
    return plug;
}

void vs_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    VS_Priv *p = (VS_Priv *)plug->priv;
    if (p) {
        /* صبر برای پایان worker thread های در حال اجرا — همون
           الگوی use-after-free که در browser_ext پیدا شد، اینجا
           هم می‌تونست رخ بده اگه vs_sniff_async در حال اجرا بود */
        DWORD wait_start = GetTickCount();
        while (p->active_workers > 0 &&
               (GetTickCount() - wait_start) < 3000) {
            Sleep(20);
        }
        if (p->active_workers > 0) {
            LOGW("video_sniffer",
                 "WARNING: %ld worker(s) still active "
                 "after timeout — proceeding with cleanup anyway",
                 (long)p->active_workers);
        }
        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}

void vs_sniff_async(IDM_PluginHooks *plug, const char *url,
                    const char *referer, const char *page_title) {
    if (!plug || !url) return;
    VS_Priv *p = (VS_Priv *)plug->priv;
    if (!p) return;

    VS_WorkCtx *ctx = (VS_WorkCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->priv = p;
    strncpy(ctx->url,     url,        VS_MAX_URL-1);
    strncpy(ctx->referer, referer ? referer : "", VS_MAX_URL-1);
    strncpy(ctx->title,   page_title ? page_title : "", 255);

    /* حیاتی: increment قبل از CreateThread، نه داخل خود worker —
       همون gap-window race که در browser_ext پیدا و رفع شد. اگه
       caller بلافاصله بعد از این تابع destroy رو صدا بزنه، باید
       active_workers از همین‌جا غیرصفر باشه وگرنه destroy ممکنه
       p رو آزاد کنه قبل از این‌که thread تازه‌ساز فرصت اجرا پیدا
       کنه. */
    InterlockedIncrement(&p->active_workers);
    HANDLE t = CreateThread(NULL, 0, vs_async_worker, ctx, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {
        InterlockedDecrement(&p->active_workers);
        free(ctx);
    }
}

int vs_best_quality(const VS_VideoInfo *info) {
    if (!info || info->quality_count == 0) return 0;
    int best = 0;
    for (int i = 1; i < info->quality_count; i++)
        if (info->qualities[i].height > info->qualities[best].height ||
            (info->qualities[i].height == info->qualities[best].height &&
             info->qualities[i].bitrate > info->qualities[best].bitrate))
            best = i;
    return best;
}

void vs_set_callback(IDM_PluginHooks *plug,
                     VS_FoundCb on_found, void *userdata) {
    if (!plug) return;
    VS_Priv *p = (VS_Priv *)plug->priv;
    if (!p) return;
    EnterCriticalSection(&p->lock);
    p->on_found = on_found;
    p->userdata = userdata;
    LeaveCriticalSection(&p->lock);
}
