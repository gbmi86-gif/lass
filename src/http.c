/* http.c  –  WinINet-based HTTP implementation */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "http.h"
#include "lass_log.h"

/* wininet linked via -lwininet in build.bat / CMakeLists.txt */

/* ── User-Agent ──
   قبلاً "[UA قدیمی]" بود — که خودش رو صریحاً به‌عنوان
   یه download manager معرفی می‌کنه. خیلی از سایت‌های میزبان فایل
   (مخصوصاً mirrorهای دانلود گیم/رپک) دقیقاً به همین دلیل با ۴۰۳
   رد می‌کنن، چون سیستم‌های anti-leech/hotlink-protectionشون رشته‌ی
   UA رو چک می‌کنن. یه UA واقعی مرورگر باعث میشه Lass مثل یه
   مرورگر معمولی دیده بشه. */
#define IDM_AGENT   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
#define BUF_SZ      8192

/* ── WinINet handle: یه handle مشترک + mutex ── */
static HINTERNET     g_inet       = NULL;
static CRITICAL_SECTION g_inet_lock;
static BOOL          g_inet_init  = FALSE;

/* callback ثبت‌شده توسط plugins_setup.c برای تزریق هدر اضافه
   (مثل Cookie:) — src/http.c چیزی از plugins/ نمی‌دونه. */
static HttpHeaderProviderFn g_header_provider = NULL;

void http_set_header_provider(HttpHeaderProviderFn fn) {
    g_header_provider = fn;
}

static HttpSessionCustomizerFn g_session_customizer = NULL;
static HttpRequestCustomizerFn g_request_customizer = NULL;
static HttpSetCookieFn         g_cookie_handler      = NULL;

void http_set_session_customizer(HttpSessionCustomizerFn fn) {
    g_session_customizer = fn;
}
void http_set_request_customizer(HttpRequestCustomizerFn fn) {
    g_request_customizer = fn;
}
void http_set_cookie_handler(HttpSetCookieFn fn) {
    g_cookie_handler = fn;
}

/* ── خوندن همه‌ی Set-Cookie های یه پاسخ و پاس دادنشون به handler ──
   WinINet چندین instance از یه هدر رو فقط از طریق پارامتر index
   (آخرین آرگومان HttpQueryInfoA) برمی‌گردونه — هر صدا زدن با یه
   index، مقدار instance بعدی رو میده تا وقتی ERROR_HTTP_HEADER_NOT_FOUND
   بگیریم. بدون این حلقه، فقط اولین Set-Cookie خونده می‌شد و بقیه
   (مثلاً یه CSRF token جدا از session cookie) گم می‌شدن. */
static void extract_set_cookies(HINTERNET hReq, struct IDM_Download *dl,
                                const char *host) {
    if (!g_cookie_handler || !hReq) return;

    char  buf[1024];
    DWORD index = 0;
    /* سقف دفاعی — هیچ پاسخ واقعی این‌قدر Set-Cookie نداره؛ فقط
       جلوی حلقه‌ی بی‌نهایت رو می‌گیره اگه رفتار WinINet با انتظار
       مستند (افزایش خودکار index) فرق کنه. */
    for (int guard = 0; guard < 64; guard++) {
        DWORD sz = sizeof(buf);
        buf[0] = '\0';
        if (!HttpQueryInfoA(hReq, HTTP_QUERY_SET_COOKIE, buf, &sz, &index)) {
            break; /* دیگه instance بیشتری نیست (یا هدر اصلاً وجود نداشت) */
        }
        if (buf[0]) g_cookie_handler(dl, host, buf);
    }
}

/* ══════════════════════════════════════════════════════════
   ⑤ ریشه‌ی واقعیِ باگ «همزمانی چندلینکی کار نمی‌کنه»:

   INTERNET_OPTION_MAX_CONNS_PER_SERVER که در get_inet_handle
   روی خودِ session handle ست می‌شد، از IE8 به بعد توسط WinINet
   نادیده گرفته می‌شه (مایکروسافت این API رو به‌خاطر یه تصمیم
   امنیتی/anti-DoS قدیمی deprecate کرد). تنها راه واقعی برای
   بالا بردن این سقف، نوشتن مستقیم توی رجیستری قبل از باز شدن
   اولین HINTERNET session‌ه.

   وقتی این سقف پیش‌فرض (تاریخاً ۲ تا ۶ به ازای هر host) رعایت
   نشه، اتفاقی که کاربر واقعاً دید دقیقاً همینه: دانلود اول
   (چندسگمنته) خودش به‌تنهایی همه‌ی connection‌های مجاز به اون
   host رو اشغال می‌کنه؛ دانلود دوم و سوم HEAD request‌شون
   (discovery) داخل خودِ WinINet صف می‌مونه — نه در صف engine ما
   — و به همین خاطر توی UI به‌نظر می‌رسه «connecting» گیر کرده‌ن.
   به‌محض pause کردن دانلود اول، یکی از connection‌های اشغال‌شده
   آزاد می‌شه و بلافاصله دانلود بعدی جلو می‌ره — دقیقاً رفتاری که
   گزارش شد.

   این باید قبل از اولین InternetOpenA کل پراسس صدا زده بشه (از
   WinMain، قبل از engine_create) وگرنه مقدار رجیستری موقع باز
   شدن session خونده نمی‌شه و اثری نداره. */
void http_init(void) {
    HKEY hk;
    const char *path = "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    DWORD max_conns = 16;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ | KEY_WRITE, &hk) == ERROR_SUCCESS) {
        DWORD cur = 0, sz = sizeof(cur), type = 0;
        BOOL need_write_1_1 = TRUE;
        if (RegQueryValueExA(hk, "MaxConnectionsPerServer", NULL, &type,
                             (BYTE *)&cur, &sz) == ERROR_SUCCESS &&
            type == REG_DWORD && cur >= max_conns) {
            need_write_1_1 = FALSE;
        }
        if (need_write_1_1) {
            RegSetValueExA(hk, "MaxConnectionsPerServer", 0, REG_DWORD,
                           (const BYTE *)&max_conns, sizeof(max_conns));
        }

        cur = 0; sz = sizeof(cur); type = 0;
        BOOL need_write_1_0 = TRUE;
        if (RegQueryValueExA(hk, "MaxConnectionsPer1_0Server", NULL, &type,
                             (BYTE *)&cur, &sz) == ERROR_SUCCESS &&
            type == REG_DWORD && cur >= max_conns) {
            need_write_1_0 = FALSE;
        }
        if (need_write_1_0) {
            RegSetValueExA(hk, "MaxConnectionsPer1_0Server", 0, REG_DWORD,
                           (const BYTE *)&max_conns, sizeof(max_conns));
        }

        RegCloseKey(hk);
    }

    /* اگه یه WinINet handle دیگه (مثلاً IE، یا یه اجرای قبلی همین
       پراسس در تئوری) از قبل باز بود، این دو تا باعث می‌شن مقدار
       جدید رجیستری بدون نیاز به لاگین/ریبوت دوباره خونده بشه.
       بی‌ضرره حتی اگه چیزی باز نباشه. */
    InternetSetOptionA(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionA(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

static HINTERNET get_inet_handle(void) {
    /* Double-checked locking pattern */
    if (g_inet) return g_inet;

    if (!g_inet_init) {
        /* اولین بار: CS رو init کن */
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0) {
            InitializeCriticalSection(&g_inet_lock);
            g_inet_init = TRUE;
        } else {
            /* منتظر بمون CS آماده بشه */
            while (!g_inet_init) Sleep(1);
        }
    }

    EnterCriticalSection(&g_inet_lock);
    if (!g_inet) {
        g_inet = InternetOpenA(IDM_AGENT,
                               INTERNET_OPEN_TYPE_PRECONFIG,
                               NULL, NULL,
                               0 /* sync — هر connection خودش async نیست */);

        if (g_inet) {
            /* حیاتی: WinINet به‌صورت پیش‌فرض تعداد connection
               همزمان به یه سرور واحد رو محدود می‌کنه (این محدودیت
               تاریخی از دوران IE میاد و می‌تونه فقط ۲ تا ۴ باشه
               بسته به نسخه‌ی ویندوز). وقتی چند دانلود از همون
               دامنه باشن (خیلی رایجه — مثلاً همه فایل‌ها روی یه
               CDN)، درخواست‌های بیشتر از این حد داخل خود WinINet
               صف می‌مونن، نه در صف engine ما. اگه این صبر کردن
               بیشتر از connect timeout (۱۵ ثانیه) طول بکشه،
               http_discover با خطا برمی‌گرده — حتی با اینکه
               مشکل شبکه‌ای واقعی نبوده، فقط WinINet داشته صف
               نگه می‌داشته. راه‌حل: این سقف رو به یه عدد معقول
               (بیشتر از max_concurrent معمول engine) افزایش بده. */
            DWORD max_conns = 16;
            InternetSetOptionA(g_inet, INTERNET_OPTION_MAX_CONNS_PER_SERVER,
                               &max_conns, sizeof(max_conns));
            InternetSetOptionA(g_inet, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER,
                               &max_conns, sizeof(max_conns));

            /* Proxy، اگه کاربر فعالش کرده باشه — یه‌بار همین‌جا،
               چون g_inet یه singleton سراسریه. قبلاً این callback
               اصلاً ثبت/صدا زده نمی‌شد. */
            if (g_session_customizer) g_session_customizer(g_inet);
        }
    }
    HINTERNET h = g_inet;
    LeaveCriticalSection(&g_inet_lock);
    return h;
}

/* ParsedURL در http.h تعریف شده — اینجا فقط parse_url implementation */
static bool parse_url(const char *url, ParsedURL *out) {
    URL_COMPONENTSA uc = {0};
    uc.dwStructSize        = sizeof(uc);
    uc.lpszScheme          = out->scheme;
    uc.dwSchemeLength      = sizeof(out->scheme);
    uc.lpszHostName        = out->host;
    uc.dwHostNameLength    = sizeof(out->host);
    uc.lpszUrlPath         = out->path;
    uc.dwUrlPathLength     = sizeof(out->path);
    uc.nPort               = 0;

    if (!InternetCrackUrlA(url, 0, 0, &uc)) return false;
    out->port = uc.nPort;
    if (out->port == 0) {
        out->port = (_stricmp(out->scheme, "https") == 0)
                    ? INTERNET_DEFAULT_HTTPS_PORT
                    : INTERNET_DEFAULT_HTTP_PORT;
    }
    return true;
}

/* HTTP_Conn struct در http.h تعریف شده */

void http_extract_host(const char *url, char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = '\0';
    if (!url) return;

    char host_buf[256] = {0};
    URL_COMPONENTSA uc = {0};
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host_buf;
    uc.dwHostNameLength = sizeof(host_buf);

    if (InternetCrackUrlA(url, 0, 0, &uc) && host_buf[0]) {
        snprintf(out, out_sz, "%s", host_buf);
    }
}

/* ======================================================
   Discovery  (HEAD request)
   ====================================================== */
IDM_Result http_discover(const char *url, const char *referer,
                         struct IDM_Download *dl, HTTP_DiscoveryInfo *out) {
    if (!url || !out) return IDM_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    HINTERNET inet = get_inet_handle();
    if (!inet) return IDM_ERR_NETWORK;

    ParsedURL pu;
    if (!parse_url(url, &pu)) return IDM_ERR_INVALID;

    DWORD flags = (pu.port == INTERNET_DEFAULT_HTTPS_PORT ||
                   _stricmp(pu.scheme, "https") == 0)
                  ? INTERNET_FLAG_SECURE : 0;
    flags |= INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD;

    HINTERNET hConn = InternetConnectA(inet, pu.host, pu.port,
                                       NULL, NULL,
                                       INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) return IDM_ERR_NETWORK;

    /* ── timeout: افزایش از ۱۵ به ۴۵ ثانیه برای connect ──
       دلیل: WinINet به‌صورت پیش‌فرض تعداد connection همزمان به
       یه سرور رو محدود می‌کنه (تاریخاً ۲ تا ۶). وقتی چند دانلود
       از همون دامنه باشن، بقیه داخل خود WinINet صف می‌مونن، نه
       در صف engine ما. سعی کردیم این سقف رو با
       INTERNET_OPTION_MAX_CONNS_PER_SERVER بالا ببریم (در
       get_inet_handle)، ولی این option در بعضی نسخه‌های ویندوز
       (از IE8 به بعد) شناخته‌شده که نادیده گرفته میشه. پس یه
       timeout سخاوتمندتر، مستقل از اون fix، تضمین می‌کنه که
       دانلودهای همزمان از یه سرور واحد به‌جای fail شدن، فقط کمی
       صبر کنن تا نوبتشون برسه. */
    DWORD to_connect = 45000, to_receive = 10000;
    InternetSetOptionA(hConn, INTERNET_OPTION_CONNECT_TIMEOUT,
                       &to_connect, sizeof(to_connect));
    InternetSetOptionA(hConn, INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &to_receive, sizeof(to_receive));

    HINTERNET hReq = HttpOpenRequestA(hConn, "HEAD", pu.path,
                                      "HTTP/1.1", NULL, NULL, flags, 0);
    if (!hReq) {
        LOGE(LOG_MOD_HTTP, "HttpOpenRequestA failed for %s (err=%lu)",
             pu.host, (unsigned long)GetLastError());
        InternetCloseHandle(hConn);
        return IDM_ERR_NETWORK;
    }

    HttpAddRequestHeadersA(hReq,
        "Accept-Encoding: identity\r\n"
        "User-Agent: " IDM_AGENT "\r\n",
        -1L,
        HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);

    if (referer && referer[0]) {
        char ref_hdr[IDM_MAX_URL + 16];
        snprintf(ref_hdr, sizeof(ref_hdr), "Referer: %s\r\n", referer);
        HttpAddRequestHeadersA(hReq, ref_hdr, -1L,
            HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    }

    if (dl && g_header_provider) {
        char extra[2048] = {0};
        int n = g_header_provider(dl, extra, (int)sizeof(extra) - 1);
        if (n > 0) {
            HttpAddRequestHeadersA(hReq, extra, -1L,
                HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
        }
    }

    /* HTTP Auth، اگه برای این host credential ذخیره‌شده داره —
       باید قبل از HttpSendRequestA اعمال بشه. قبلاً این callback
       اصلاً ثبت/صدا زده نمی‌شد. */
    if (g_request_customizer) g_request_customizer(dl, hReq, pu.host);

    IDM_Result result = IDM_OK;

    if (!HttpSendRequestA(hReq, NULL, 0, NULL, 0)) {
        /* حیاتی برای دیباگ آینده: بدون این لاگ، هیچ‌وقت نمی‌فهمیم
           آیا شکست واقعاً به‌خاطر مشکل شبکه بوده یا صف WinINet.
           ERROR_INTERNET_TIMEOUT (12002) یا
           ERROR_INTERNET_CANNOT_CONNECT (12029) نشونه‌ی
           تایم‌اوت/صف هستن؛ کدهای دیگه معمولاً مشکل واقعی شبکه‌ان. */
        DWORD err = GetLastError();
        LOGE(LOG_MOD_HTTP,
             "HttpSendRequestA failed for %s%s (err=%lu%s)",
             pu.host, pu.path, (unsigned long)err,
             (err == ERROR_INTERNET_TIMEOUT || err == ERROR_INTERNET_CANNOT_CONNECT)
                 ? " — likely WinINet per-server connection queue/timeout"
                 : "");
        result = IDM_ERR_NETWORK;
        goto cleanup;
    }

    /* حتی اگه بعداً status رد بشه (مثلاً 403/429 موقت)، سرور ممکنه
       یه cookie لازم برای تلاش بعدی ست کرده باشه — پس همیشه، قبل
       از هر چک status ای، استخراجش می‌کنیم. */
    extract_set_cookies(hReq, dl, pu.host);

    DWORD status = 0, sz = sizeof(status);
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &sz, NULL);
    if (status < 200 || status >= 400) {
        /* تمایز بین خطای دائمی (retry بی‌فایده‌ست) و موقتی
           (ارزش retry داره):
           - 404/410: منبع واقعاً وجود نداره — retry هیچ‌وقت
             جواب نمیده، فقط وقت کاربر رو تلف می‌کنه
           - 400/401: درخواست بدشکل یا نیاز به احراز هویت —
             تکرار همون درخواست بدون تغییر چیزی حل نمیشه
           - 403/429: می‌تونه rate-limiting موقتی باشه (دقیقاً
             سناریویی که با چند دانلود همزمان از یه سرور دیدیم)
             — ارزش retry داره
           - 5xx: مشکل سمت سرور، معمولاً موقتیه — ارزش retry داره */
        BOOL permanent = (status == 404 || status == 410 ||
                          status == 400 || status == 401);

        LOGW(LOG_MOD_HTTP,
             "Server rejected %s%s with HTTP %lu%s%s",
             pu.host, pu.path, (unsigned long)status,
             (status == 403 || status == 429)
                 ? " — server may be limiting concurrent connections from this client"
                 : "",
             permanent ? " — permanent error, will not retry" : "");

        /* ── گفتگو، نه دیکته ──
           اگه سرور روی یه 429 (Too Many Requests) یا 503 (Service
           Unavailable) صراحتاً Retry-After فرستاده، این دقیقاً
           سرور داره بهمون میگه «الان نه، این‌قدر صبر کن». نادیده
           گرفتنش (و به‌جاش استفاده از backoff نمایی خودمون که
           می‌تونه خیلی کوتاه‌تر یا بلندتر از چیزی باشه که سرور
           واقعاً می‌خواد) دقیقاً همون «دیکته‌کردن» به‌جای «گفتگو»ست.
           فقط فرم delta-seconds رو می‌پذیریم (فرم HTTP-date نادره؛
           نادیده گرفتنش امن‌تر از parse اشتباهه). */
        if (status == 429 || status == 503) {
            char ra[32] = {0};
            DWORD ra_sz = sizeof(ra);
            if (HttpQueryInfoA(hReq, HTTP_QUERY_RETRY_AFTER, ra, &ra_sz, NULL) &&
                ra[0] && isdigit((unsigned char)ra[0])) {
                long sec = atol(ra);
                if (sec > 0 && sec <= 3600) { /* سقف معقول یه ساعت */
                    out->retry_after_sec = (uint32_t)sec;
                    LOGI(LOG_MOD_HTTP,
                         "server requested Retry-After: %ld sec for %s",
                         sec, pu.host);
                }
            }
        }

        result = permanent ? IDM_ERR_NOT_FOUND : IDM_ERR_NETWORK;
        goto cleanup;
    }

    char hdr_val[64];
    sz = sizeof(hdr_val);
    if (HttpQueryInfoA(hReq, HTTP_QUERY_CONTENT_LENGTH, hdr_val, &sz, NULL)) {
        /* حیاتی: _atoi64 می‌تونه مقدار منفی برگردونه (اگه هدر
           malformed باشه، مثلاً "-1" یا یه رشته‌ی غیرعددی). وقتی
           این به uint64_t cast بشه، یه عدد نجومی نزدیک به سقف
           uint64 میشه که بعداً برای preallocate کردن فایل استفاده
           میشه (fw_open → SetEndOfFile) — این می‌تونه یه تلاش
           برای denial-of-service باشه (سرور مخرب سعی کنه باعث
           بشه Lass یه فایل غیرممکن بزرگ preallocate کنه).
           سقف معقول: 10 ترابایت — هیچ دانلود واقعی از این بزرگ‌تر
           نیست، و رد کردنش امن‌تر از قبول کوره‌ای هست. */
        LONGLONG raw = _atoi64(hdr_val);
        const LONGLONG MAX_REASONABLE_SIZE = 10LL * 1024 * 1024 * 1024 * 1024; /* 10TB */
        if (raw < 0 || raw > MAX_REASONABLE_SIZE) {
            LOGW(LOG_MOD_HTTP,
                 "Content-Length out of sane range (%lld) — ignoring, "
                 "treating as unknown size", (long long)raw);
            out->content_length = 0;
        } else {
            out->content_length = (uint64_t)raw;
        }
    }

    /* ── Accept-Ranges: درست خوندن header دلخواه ──
       باگ قبلی: HTTP_QUERY_CUSTOM بدون مقداردهی hdr_val به نام
       header، یعنی این query همیشه fail می‌شد و کد به‌اشتباه
       فرض می‌کرد "اگه Content-Length هست پس Range هم ساپورت میشه"
       — این دقیقاً چیزی بود که باعث segmentation روی سرورهایی که
       واقعاً Range request رو پشتیبانی نمی‌کردن می‌شد، و نتیجه‌اش
       چندین thread که همزمان از offset صفر می‌نوشتن → فایل خراب،
       مخصوصاً در فرمت‌های حساس مثل ZIP. */
    char accept_ranges_hdr[64] = "Accept-Ranges";
    sz = sizeof(accept_ranges_hdr);
    out->accept_ranges = false;
    if (HttpQueryInfoA(hReq, HTTP_QUERY_CUSTOM, accept_ranges_hdr, &sz, NULL)) {
        out->accept_ranges = (_stricmp(accept_ranges_hdr, "bytes") == 0);
    }
    /* اگه header موجود نبود، دیگه فرض pessimistic-defaultی نمی‌کنیم —
       موقع گرفتن segment اول با یه range-test واقعی (GET با Range
       header و چک status 206) صحت رو تأیید می‌کنیم؛ اینجا فقط چیزی
       که سرور واقعاً اعلام کرده رو ثبت می‌کنیم. */

    sz = sizeof(out->content_type);
    HttpQueryInfoA(hReq, HTTP_QUERY_CONTENT_TYPE, out->content_type, &sz, NULL);

    sz = sizeof(out->etag);
    HttpQueryInfoA(hReq, HTTP_QUERY_ETAG, out->etag, &sz, NULL);

    sz = sizeof(out->last_modified);
    HttpQueryInfoA(hReq, HTTP_QUERY_LAST_MODIFIED, out->last_modified, &sz, NULL);

    out->is_https = (_stricmp(pu.scheme, "https") == 0);

    /* Content-Disposition رو بخون تا اسم واقعی فایل رو داشته باشیم.
       بعضی CDN ها URL توکن‌دار میدن ولی در Content-Disposition
       اسم واقعی رو اعلام می‌کنن (مثل: filename="uabea-windows.zip") */
    char content_disp[512] = {0};
    DWORD cd_sz = sizeof(content_disp) - 1;
    /* HTTP_QUERY_CUSTOM نیاز داره اسم هدر رو در خود بافر بذاریم */
    strncpy(content_disp, "Content-Disposition", sizeof(content_disp) - 1);
    if (!HttpQueryInfoA(hReq, HTTP_QUERY_CUSTOM, content_disp, &cd_sz, NULL))
        content_disp[0] = '\0';

    http_extract_filename(url,
                          content_disp[0] ? content_disp : NULL,
                          out->filename, sizeof(out->filename));

cleanup:
    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    return result;
}

/* ======================================================
   Open a ranged HTTP connection for a segment
   ====================================================== */
HTTP_Conn *http_open_range(const char *url, const char *referer,
                           struct IDM_Download *dl,
                           uint64_t start, uint64_t end) {
    if (!url) return NULL;

    HINTERNET inet = get_inet_handle();
    if (!inet) return NULL;

    HTTP_Conn *conn = (HTTP_Conn *)calloc(1, sizeof(HTTP_Conn));
    if (!conn) return NULL;

    if (!parse_url(url, &conn->pu)) { free(conn); return NULL; }

    DWORD flags = (_stricmp(conn->pu.scheme, "https") == 0)
                  ? INTERNET_FLAG_SECURE : 0;
    flags |= INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
             INTERNET_FLAG_KEEP_CONNECTION;

    conn->hConnect = InternetConnectA(inet, conn->pu.host, conn->pu.port,
                                      NULL, NULL,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn->hConnect) { free(conn); return NULL; }

    /* ── timeout ──
       قبلاً ۱۵۰۰۰/۶۰۰۰۰ بود. تست واقعی نشون داد تلاش برای cancel
       کردن I/O بلاک‌شده از یه thread دیگه (با InternetCloseHandle)
       روی WinINet قابل‌اعتماد نیست و حتی باعث یه use-after-free
       واقعی شد (رجوع کن به تاریخچه‌ی segment.c). راه‌حل امن: یه
       receive-timeout کوتاه که خود worker thread رو، بدون هیچ
       دخالت بیرونی، در چند ثانیه به cancel_flag برسونه. عوارض
       جانبی این کار، یه reconnect گاه‌به‌گاه روی شبکه‌ی واقعاً کند
       (نه stall شده) هست که منطق retry موجود (SEG_MAX_RETRY با
       backoff) به‌هرحال پوششش می‌ده. */
    DWORD to_connect = 8000, to_receive = 6000;
    InternetSetOptionA(conn->hConnect, INTERNET_OPTION_CONNECT_TIMEOUT,
                       &to_connect, sizeof(to_connect));
    InternetSetOptionA(conn->hConnect, INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &to_receive, sizeof(to_receive));

    conn->hRequest = HttpOpenRequestA(conn->hConnect, "GET", conn->pu.path,
                                      "HTTP/1.1", NULL, NULL, flags, 0);
    if (!conn->hRequest) {
        InternetCloseHandle(conn->hConnect);
        free(conn);
        return NULL;
    }

    /* Set Range header */
    char range_hdr[128];
    if (end > 0)
        snprintf(range_hdr, sizeof(range_hdr),
                 "Range: bytes=%llu-%llu\r\n",
                 (unsigned long long)start, (unsigned long long)end);
    else
        snprintf(range_hdr, sizeof(range_hdr),
                 "Range: bytes=%llu-\r\n",
                 (unsigned long long)start);

    HttpAddRequestHeadersA(conn->hRequest, range_hdr, -1L,
                           HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    HttpAddRequestHeadersA(conn->hRequest,
                           "User-Agent: " IDM_AGENT "\r\n",
                           -1L,
                           HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    if (referer && referer[0]) {
        char ref_hdr[IDM_MAX_URL + 16];
        snprintf(ref_hdr, sizeof(ref_hdr), "Referer: %s\r\n", referer);
        HttpAddRequestHeadersA(conn->hRequest, ref_hdr, -1L,
            HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    }

    if (dl && g_header_provider) {
        char extra[2048] = {0};
        int n = g_header_provider(dl, extra, (int)sizeof(extra) - 1);
        if (n > 0) {
            HttpAddRequestHeadersA(conn->hRequest, extra, -1L,
                HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
        }
    }

    if (g_request_customizer)
        g_request_customizer(dl, conn->hRequest, conn->pu.host);

    if (!HttpSendRequestA(conn->hRequest, NULL, 0, NULL, 0)) {
        InternetCloseHandle(conn->hRequest);
        InternetCloseHandle(conn->hConnect);
        free(conn);
        return NULL;
    }

    /* همون منطق discover — سرور ممکنه در پاسخ یه GET رنج‌دار هم
       کوکی تازه ست کنه (بعضی CDN ها token رو فقط موقع اولین GET
       واقعی، نه HEAD، می‌فرستن). */
    extract_set_cookies(conn->hRequest, dl, conn->pu.host);

    DWORD status = 0, sz = sizeof(status);
    HttpQueryInfoA(conn->hRequest,
                   HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &sz, NULL);
    /* 206 = partial content (range واقعاً honor شده)
       200 = سرور Range رو نادیده گرفته و کل فایل رو از صفر می‌فرسته */
    if (status != 206 && status != 200) {
        InternetCloseHandle(conn->hRequest);
        InternetCloseHandle(conn->hConnect);
        free(conn);
        return NULL;
    }
    conn->got_partial = (status == 206);

    conn->range_start = start;
    conn->range_end   = end;
    conn->bytes_read  = 0;
    conn->open        = true;
    return conn;
}

/* ======================================================
   Read data from open connection
   ====================================================== */
bool http_conn_got_partial(const HTTP_Conn *conn) {
    return conn ? conn->got_partial : false;
}

int http_read(HTTP_Conn *conn, void *buf, int buf_size) {
    if (!conn || !conn->open || !buf || buf_size <= 0) return -1;

    DWORD bytes_read = 0;
    if (!InternetReadFile(conn->hRequest, buf, (DWORD)buf_size, &bytes_read)) {
        conn->open = false;
        return -1;
    }

    if (bytes_read == 0) {
        conn->open = false;
        return 0; /* EOF */
    }

    conn->bytes_read += bytes_read;
    return (int)bytes_read;
}

/* ======================================================
   Close connection
   ====================================================== */
void http_close(HTTP_Conn *conn) {
    if (!conn) return;
    if (conn->hRequest) InternetCloseHandle(conn->hRequest);
    if (conn->hConnect) InternetCloseHandle(conn->hConnect);
    if (conn->hSession) InternetCloseHandle(conn->hSession); /* FTP */
    conn->open = false;
    free(conn);
}

/* ======================================================
   Extract filename from URL or Content-Disposition
   ====================================================== */
/* حذف کاراکترهای خطرناک از filename که از منابع untrusted (سرور،
   Content-Disposition header) میاد. این جلوی چند نوع حمله رو
   می‌گیره:
   - Path Traversal: "../../../Windows/System32/evil.dll" یا
     "..\\..\\AppData\\Startup\\malware.exe" — سرور مخرب می‌تونه
     سعی کنه با filename دستکاری‌شده بیرون از پوشه‌ی دانلود
     بنویسه (چون این filename بعداً در engine.c مستقیم به
     out_dir + filename ترکیب میشه).
   - Absolute path injection: "C:\Windows\System32\evil.dll" یا
     "\\server\share\file" (UNC) — اگه filename خودش یه مسیر
     absolute باشه، ترکیب با دایرکتوری دانلود بی‌معنی میشه و
     می‌تونه رفتار غیرمنتظره بده.
   - کاراکترهای کنترلی/null byte injection.
   خروجی: فقط نام فایل خالص، بدون هیچ separator یا drive letter. */
static void sanitize_filename(char *fn, int fn_sz) {
    if (!fn || fn_sz <= 0 || !fn[0]) return;

    /* حذف کاراکترهای کنترلی (شامل null byte میانی که استرینگ رو
       زودتر از موعد قطع می‌کنه و بقیه رو مخفی نگه می‌داره) */
    for (int i = 0; fn[i]; i++) {
        unsigned char c = (unsigned char)fn[i];
        if (c < 0x20 || c == 0x7F) fn[i] = '_';
    }

    /* اگه شامل separator مسیر (/ یا \) هست، فقط بخش آخر رو نگه دار
       — این خودش کل مسیر رو به یه filename خالص تبدیل می‌کنه و
       ../ های میانی رو هم بی‌اثر می‌کنه چون فقط segment آخر
       باقی می‌مونه. */
    char *last_sep = NULL;
    for (char *p = fn; *p; p++)
        if (*p == '/' || *p == '\\') last_sep = p;
    if (last_sep) {
        char tmp[512];
        strncpy(tmp, last_sep + 1, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        strncpy(fn, tmp, fn_sz - 1);
        fn[fn_sz - 1] = '\0';
    }

    /* حذف drive letter احتمالی که از segment آخر عبور کرده باشه
       (مثلاً اگه ورودی "C:evil.exe" بدون separator بود) */
    if (fn[0] && fn[1] == ':') {
        memmove(fn, fn + 2, strlen(fn + 2) + 1);
    }

    /* حذف ".." های تکی که ممکنه به‌عنوان کل filename باقی مونده
       باشن (بعد از strip بالا، اگه چیزی مثل ".." تنها مونده بود) */
    if (strcmp(fn, "..") == 0 || strcmp(fn, ".") == 0 || fn[0] == '\0') {
        strncpy(fn, "download.bin", fn_sz - 1);
        fn[fn_sz - 1] = '\0';
    }

    /* حذف کاراکترهای غیرمجاز در نام فایل ویندوز:
       < > : " | ? * (این‌ها می‌تونن باعث خطای CreateFile بشن یا
       در تئوری برای command injection در برخی shell ها استفاده
       بشن، هرچند ما مستقیم به CreateFile می‌دیم نه shell) */
    for (int i = 0; fn[i]; i++) {
        char c = fn[i];
        if (c=='<'||c=='>'||c==':'||c=='"'||c=='|'||c=='?'||c=='*')
            fn[i] = '_';
    }

    /* trim فضای خالی و نقطه از انتها — ویندوز این‌ها رو نادیده
       می‌گیره ولی می‌تونن باعث ابهام در نمایش UI بشن */
    int len = (int)strlen(fn);
    while (len > 0 && (fn[len-1] == ' ' || fn[len-1] == '.')) {
        fn[--len] = '\0';
    }
    if (len == 0) strncpy(fn, "download.bin", fn_sz - 1);
}

void http_extract_filename(const char *url, const char *content_disp,
                           char *out, int out_len) {
    if (!out || out_len <= 0) return;
    out[0] = '\0';

    /* Try Content-Disposition: attachment; filename="foo.zip" */
    if (content_disp) {
        const char *fn = strstr(content_disp, "filename=");
        if (fn) {
            fn += 9;
            if (*fn == '"') fn++;
            int i = 0;
            while (*fn && *fn != '"' && *fn != '\r' && *fn != '\n' && i < out_len - 1)
                out[i++] = *fn++;
            out[i] = '\0';
            if (out[0]) {
                sanitize_filename(out, out_len);
                return;
            }
        }
    }

    /* Fall back: last path segment in URL */
    if (!url) { strcpy(out, "download.bin"); return; }

    const char *last_slash = url;
    for (const char *p = url; *p; p++)
        if (*p == '/' || *p == '\\') last_slash = p + 1;

    /* Strip query string */
    int i = 0;
    for (const char *p = last_slash; *p && *p != '?' && *p != '#' && i < out_len - 1; p++)
        out[i++] = *p;
    out[i] = '\0';

    if (out[0] == '\0') strcpy(out, "download.bin");
    sanitize_filename(out, out_len);
}

/* ======================================================
   Mime-type → extension mapping
   وقتی URL بدون پسوند هست (مثل لینک‌های توکن‌دار CDN) و سرور
   هیچ Content-Disposition نمی‌فرسته، اینجا کمک می‌کنه فایل با
   پسوند درست ذخیره بشه نه به شکل یه رشته بی‌معنی.
   ====================================================== */
typedef struct { const char *mime; const char *ext; } MimeExt;

static const MimeExt MIME_TABLE[] = {
    /* ── ویدیو ── */
    {"video/mp4",              ".mp4"},
    {"video/x-matroska",       ".mkv"},
    {"video/webm",             ".webm"},
    {"video/x-msvideo",        ".avi"},
    {"video/quicktime",        ".mov"},
    {"video/x-ms-wmv",         ".wmv"},
    {"video/x-flv",            ".flv"},
    {"video/mp2t",             ".ts"},
    {"video/3gpp",             ".3gp"},
    {"application/vnd.apple.mpegurl", ".m3u8"},
    {"application/dash+xml",   ".mpd"},

    /* ── صوت ── */
    {"audio/mpeg",             ".mp3"},
    {"audio/flac",             ".flac"},
    {"audio/aac",              ".aac"},
    {"audio/ogg",              ".ogg"},
    {"audio/wav",              ".wav"},
    {"audio/x-wav",            ".wav"},
    {"audio/mp4",              ".m4a"},
    {"audio/x-ms-wma",         ".wma"},

    /* ── سند ── */
    {"application/pdf",        ".pdf"},
    {"application/msword",     ".doc"},
    {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx"},
    {"application/vnd.ms-excel", ".xls"},
    {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", ".xlsx"},
    {"application/vnd.ms-powerpoint", ".ppt"},
    {"application/vnd.openxmlformats-officedocument.presentationml.presentation", ".pptx"},
    {"text/plain",             ".txt"},
    {"application/epub+zip",   ".epub"},

    /* ── نرم‌افزار / آرشیو ── */
    {"application/x-msdownload",      ".exe"},
    {"application/x-msdos-program",   ".exe"},
    {"application/vnd.microsoft.portable-executable", ".exe"},
    {"application/x-msi",             ".msi"},
    {"application/x-apple-diskimage", ".dmg"},
    {"application/vnd.android.package-archive", ".apk"},
    {"application/zip",               ".zip"},
    {"application/x-rar-compressed",  ".rar"},
    {"application/vnd.rar",           ".rar"},
    {"application/x-7z-compressed",   ".7z"},
    {"application/x-tar",             ".tar"},
    {"application/gzip",              ".gz"},
    {"application/x-gzip",            ".gz"},
    {"application/x-bzip2",           ".bz2"},
    {"application/x-iso9660-image",   ".iso"},
    {"application/octet-stream",      ".bin"}, /* fallback عمومی */

    /* ── تصویر ── */
    {"image/jpeg",             ".jpg"},
    {"image/png",              ".png"},
    {"image/gif",              ".gif"},
    {"image/bmp",              ".bmp"},
    {"image/svg+xml",          ".svg"},
    {"image/webp",             ".webp"},
};
#define MIME_TABLE_COUNT ((int)(sizeof(MIME_TABLE)/sizeof(MIME_TABLE[0])))

/* بررسی اینکه filename از قبل یه پسوند معقول داره
   (یعنی نقطه‌ای داره که بعدش ۱ تا ۵ کاراکتر حرفی‌عددی باشه و
   انتهای رشته باشه — نه یه نقطه وسط یه شناسه عجیب) */
static BOOL has_valid_extension(const char *filename) {
    if (!filename || !filename[0]) return FALSE;
    const char *dot = strrchr(filename, '.');
    if (!dot) return FALSE;
    const char *p = dot + 1;
    int len = 0;
    while (*p) {
        if (!isalnum((unsigned char)*p)) return FALSE;
        p++; len++;
        if (len > 6) return FALSE; /* پسوندهای واقعی معمولاً کوتاهن */
    }
    return (len >= 1);
}

void http_fixup_extension(char *filename, int filename_sz,
                          const char *content_type) {
    if (!filename || filename_sz <= 0) return;
    if (has_valid_extension(filename)) return; /* از قبل پسوند دارد */
    if (!content_type || !content_type[0]) return;

    /* جدا کردن mime اصلی از پارامترهای اضافی مثل charset */
    char mime_clean[128] = {0};
    strncpy(mime_clean, content_type, sizeof(mime_clean) - 1);
    char *semi = strchr(mime_clean, ';');
    if (semi) *semi = '\0';
    /* trim trailing space */
    int mlen = (int)strlen(mime_clean);
    while (mlen > 0 && mime_clean[mlen-1] == ' ') mime_clean[--mlen] = '\0';

    const char *ext = NULL;
    for (int i = 0; i < MIME_TABLE_COUNT; i++) {
        if (_stricmp(mime_clean, MIME_TABLE[i].mime) == 0) {
            ext = MIME_TABLE[i].ext;
            break;
        }
    }
    if (!ext) return; /* mime ناشناخته — تغییری نمی‌دیم */

    int cur_len = (int)strlen(filename);
    int ext_len = (int)strlen(ext);
    if (cur_len + ext_len >= filename_sz) {
        /* جا نیست — انتهای نام رو کوتاه کن تا پسوند جا بشه */
        cur_len = filename_sz - ext_len - 1;
        if (cur_len < 1) return;
        filename[cur_len] = '\0';
    }
    strncat(filename, ext, (size_t)(filename_sz - cur_len - 1));
}
