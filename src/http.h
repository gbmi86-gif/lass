/* http.h  –  HTTP abstraction over WinINet (no libcurl dependency) */
#ifndef IDM_HTTP_H
#define IDM_HTTP_H

#include "types.h"
#include <wininet.h>

/* ── ParsedURL: public — plugin_ftp هم استفاده می‌کنه ── */
typedef struct {
    char          scheme[16];
    char          host[512];
    INTERNET_PORT port;
    char          path[IDM_MAX_URL];
} ParsedURL;

/* Info returned by a HEAD request */
typedef struct {
    uint64_t content_length;
    bool     accept_ranges;
    bool     is_https;
    char     content_type[128];
    char     filename[IDM_MAX_FILENAME];
    char     etag[128];
    char     last_modified[64];
    /* اگه سرور روی 429/503 صراحتاً Retry-After (به‌فرم delta-seconds)
       فرستاده باشه، اینجا ثبت میشه؛ 0 یعنی چیزی نگفته. این دقیقاً
       تفاوت «دیکته کردن» و «گفتگو کردن»: به‌جای اینکه خودمون حدس
       بزنیم چقدر باید صبر کنیم، اگه سرور صریحاً گفته باشه، همونو
       رعایت می‌کنیم. */
    uint32_t retry_after_sec;
} HTTP_DiscoveryInfo;

/* Per-segment HTTP/FTP context  –  تعریف کامل public هست
   چون plugin_ftp.c مستقیم به fields نیاز داره             */
struct HTTP_Conn {
    HINTERNET hConnect;
    HINTERNET hRequest;
    HINTERNET hSession;   /* FTP: session handle */
    ParsedURL pu;
    uint64_t  range_start;
    uint64_t  range_end;
    uint64_t  bytes_read;
    bool      open;
    bool      is_ftp;       /* TRUE اگه FTP connection باشه */
    bool      got_partial;  /* TRUE اگه سرور واقعاً 206 برگردونده —
                               یعنی Range request رو honor کرده.
                               اگه start>0 بود ولی این FALSE شد،
                               یعنی سرور کل فایل رو از صفر می‌فرسته
                               و این segment باید reject بشه وگرنه
                               فایل خراب می‌شه. */
};
typedef struct HTTP_Conn HTTP_Conn;

/* forward declare — src/ نباید به plugins/ وابسته بشه، پس فقط
   pointer لازم داریم، نه تعریف کامل IDM_Download. */
struct IDM_Download;

/* ── تزریق هدر اضافه از پلاگین‌ها (مثل Cookie: از Cookie Jar) ──
   src/http.c مستقیم چیزی از plugins/ نمی‌دونه (لایه‌بندی عمدیه) —
   به‌جاش، لایه‌ی بالاتر (plugins_setup.c) یه callback این‌جا ثبت
   می‌کنه. قبلاً مکانیزم جمع‌آوری هدر (plugin_collect_http_headers)
   تعریف شده بود ولی هیچ‌جا از http.c صدا زده نمی‌شد — یعنی Cookie
   Jar هیچ‌وقت هیچ کوکی‌ای واقعاً نمی‌فرستاد. */
typedef int (*HttpHeaderProviderFn)(struct IDM_Download *dl,
                                    char *buf, int max_len);
void http_set_header_provider(HttpHeaderProviderFn fn);

/* ── تزریق تنظیمات Proxy، یه‌بار وقتی WinINet session ساخته میشه ──
   src/http.c از plugins/ چیزی نمی‌دونه، پس plugins_setup.c این‌جا
   یه callback ثبت می‌کنه که خودش داخلش proxy_plugin_apply رو صدا
   می‌زنه. محدودیت شناخته‌شده: چون session یه singleton هست که فقط
   یه‌بار ساخته میشه، تغییر تنظیمات proxy در حین اجرای برنامه تا
   restart بعدی اعمال نمیشه — قابل قبول، رفتار رایج خیلی از
   برنامه‌هاست. */
typedef void (*HttpSessionCustomizerFn)(HINTERNET hSession);
void http_set_session_customizer(HttpSessionCustomizerFn fn);

/* ── تزریق credential احراز هویت HTTP، به‌ازای هر request (HEAD یا
   GET) — بعد از HttpOpenRequestA و قبل از HttpSendRequestA. این
   دو callback (و header_provider بالا) قبلاً وجود داشتن (توابع
   auth_plugin_apply/proxy_plugin_apply در plugin_http_auth.c و
   plugin_proxy.c) ولی هیچ‌جا واقعاً صدا زده نمی‌شدن — یعنی تنظیمات
   Proxy و HTTP Auth که کاربر توی UI ست می‌کرد، هیچ‌وقت واقعاً روی
   دانلود واقعی اعمال نمی‌شد. */
typedef void (*HttpRequestCustomizerFn)(struct IDM_Download *dl,
                                        HINTERNET hReq, const char *host);
void http_set_request_customizer(HttpRequestCustomizerFn fn);

/* ── دریافت Set-Cookie از پاسخ سرور، به‌ازای هر request (HEAD یا
   GET) — بعد از HttpSendRequestA. یه پاسخ می‌تونه چندین
   Set-Cookie جدا داشته باشه (مثلاً session + CSRF token با هم)،
   پس این callback ممکنه چندبار — یه‌بار به‌ازای هر instance —
   برای همون request صدا زده بشه.

   قبل از این، cookie_jar فقط نیمه‌ی «فرستادن» round-trip رو
   داشت (on_http_headers → Cookie: در درخواست) ولی هیچ راهی برای
   واقعاً *یاد گرفتن* کوکی‌ای که سرور در پاسخ ست می‌کنه نداشت —
   یعنی برای هر سایتی که دانلود واقعی نیاز به یه session/auth
   cookie تازه داره (خیلی رایج در CDN/مirror‌های نیازمند توکن)،
   cookie jar عملاً بی‌اثر بود مگر کاربر دستی cookies.txt وارد
   کرده باشه. */
typedef void (*HttpSetCookieFn)(struct IDM_Download *dl,
                                const char *host,
                                const char *set_cookie_value);
void http_set_cookie_handler(HttpSetCookieFn fn);

/* ── باید یه‌بار، قبل از اولین InternetOpenA کل پراسس صدا زده بشه
   (یعنی قبل از engine_create) ──
   سقف WinINet برای تعداد connection همزمان به یه host واحد رو از
   طریق رجیستری بالا می‌بره؛ رفع ریشه‌ای مشکل «چند دانلود مختلف
   از یه سرور، فقط یکیشون واقعاً پیش می‌ره». جزئیات کامل در
   http.c بالای پیاده‌سازی. */
void http_init(void);

/* ---- Discovery ----
   referer: اختیاری، NULL یا "" یعنی هیچ Referer header‌ای فرستاده
   نمیشه. بعضی سایت‌های میزبان فایل بدون Referer معتبر با ۴۰۳ رد
   می‌کنن (hotlink protection) — این ربطی به تعداد segment/thread
   نداره. */
IDM_Result http_discover(const char *url, const char *referer,
                         struct IDM_Download *dl, HTTP_DiscoveryInfo *out);

/* استخراج فقط hostname از URL (بدون scheme/port/path) — برای
   منطق per-host staggering در engine.c، تا موتور بتونه تشخیص
   بده چند دانلود مختلف به یه سرور واحد میرن. */
void http_extract_host(const char *url, char *out, int out_sz);

/* ---- Segment download ---- */
HTTP_Conn *http_open_range(const char *url, const char *referer,
                           struct IDM_Download *dl,
                           uint64_t start, uint64_t end);
int        http_read      (HTTP_Conn *conn, void *buf, int buf_size);
void       http_close     (HTTP_Conn *conn);

/* TRUE اگه سرور واقعاً 206 Partial Content برگردونده — یعنی
   Range request honor شده. اگه segment با start>0 این FALSE
   برگردوند، یعنی سرور کل فایل رو از صفر می‌فرسته و segment
   باید reject بشه تا فایل خراب نشه. */
bool http_conn_got_partial(const HTTP_Conn *conn);

/* ---- Helpers ---- */
void http_extract_filename(const char *url, const char *content_disp,
                           char *out, int out_len);

/* ── اگه filename پسوند نداشت، از content_type یه پسوند مناسب اضافه کن ──
   مثال: filename="9f8e7d6c" + content_type="video/mp4" → "9f8e7d6c.mp4"
   اگه filename از قبل پسوند داشت یا content_type ناشناخته بود، تغییری نمیده */
void http_fixup_extension(char *filename, int filename_sz,
                          const char *content_type);

#endif /* IDM_HTTP_H */
