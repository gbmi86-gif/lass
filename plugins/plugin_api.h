/* plugin_api.h
   ─────────────────────────────────────────────────────────────
   رابط پلاگین برای Lass
   کد اصلی (src/) هیچ‌وقت تغییر نمی‌کنه.
   هر پلاگین فقط این هدر رو include می‌کنه و
   از طریق IDM_PluginHooks به موتور وصل می‌شه.
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_API_H
#define IDM_PLUGIN_API_H

#include "../src/types.h"
#include "../src/engine.h"
#include "../src/segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================
   نسخه API پلاگین
   ====================================================== */
#define IDM_PLUGIN_API_VERSION  1

/* ======================================================
   IDM_PluginHooks
   ساختار اصلی که هر پلاگین پر می‌کنه.
   فیلدهای NULL = پلاگین آن hook رو پیاده نکرده.
   ====================================================== */
typedef struct IDM_PluginHooks {

    /* ---- شناسه پلاگین ---- */
    const char *name;       /* "smart_resume" */
    const char *version;    /* "1.0" */
    int         api_version; /* IDM_PLUGIN_API_VERSION */

    /* ---- چرخه حیات ---- */
    /* فراخوانی یک‌بار هنگام attach */
    int  (*on_init)   (void);
    /* فراخوانی یک‌بار هنگام detach */
    void (*on_destroy)(void);

    /* ---- رویدادهای دانلود ---- */
    /* قبل از شروع دانلود – می‌تونه out_path یا تعداد segment رو تغییر بده */
    void (*on_download_start)(IDM_Download *dl);
    /* هر بار که دانلود کامل شد */
    void (*on_download_done) (IDM_Download *dl);
    /* هر بار که دانلود pause شد */
    void (*on_download_pause)(IDM_Download *dl);
    /* هر بار که دانلود fail شد */
    void (*on_download_fail) (IDM_Download *dl);

    /* ---- رویدادهای segment ---- */
    /* بعد از اینکه segment worker یک chunk نوشت
       bytes_written = تعداد بایت‌های نوشته‌شده در این چرخه */
    void (*on_segment_progress)(IDM_Segment *seg, uint64_t bytes_written);
    /* وقتی یک segment کامل شد */
    void (*on_segment_done)    (IDM_Segment *seg);

    /* ---- hook اختیاری برای HTTP headers ----
       پلاگین می‌تونه header اضافه کنه (مثلاً Cookie)
       buf: بافر header اضافی، max_len: حداکثر اندازه
       مقدار برگشتی: تعداد بایت نوشته‌شده در buf */
    int  (*on_http_headers)(IDM_Download *dl,
                             char *buf, int max_len);

    /* ---- hook اختیاری برای Set-Cookie دریافتی از سرور ----
       بعد از هر HttpSendRequestA موفق (HEAD یا GET رنج‌دار) صدا
       زده میشه، یه‌بار به‌ازای هر instance جدای Set-Cookie در
       پاسخ. raw_set_cookie رشته‌ی خامیه که بعد از "Set-Cookie: "
       میاد (مثلاً "sid=abc123; Path=/; Max-Age=3600; HttpOnly").
       این نیمه‌ی «دریافت» round-trip کوکیه — on_http_headers بالا
       نیمه‌ی «فرستادن» هست. */
    void (*on_set_cookie)(IDM_Download *dl, const char *host,
                          const char *raw_set_cookie);

    /* ---- داده خصوصی پلاگین (موتور دست نمی‌زنه) ---- */
    void *priv;

} IDM_PluginHooks;

/* ======================================================
   IDM_PluginRegistry
   لیست پلاگین‌های فعال که به engine وصل شدن
   ====================================================== */
/* IDM_MAX_PLUGINS باید حداقل به تعداد پلاگین‌های واقعی پروژه باشه.
   در حال حاضر ۱۷ پلاگین داریم (api, smart_resume, dynamic_seg,
   cookie_jar, chmml, speed_graph, clipboard, basket, ftp, hls,
   proxy, http_auth, browser_ext, dash, notify, categories, rtmp,
   video_sniffer) — مقدار قبلی ۸ بود که باعث می‌شد پلاگین‌های
   نهم به بعد در plugin_attach به‌خاطر reg->count >= IDM_MAX_PLUGINS
   به‌سادگی silently رد بشن (بدون خطای صریح، فقط return -1)،
   یعنی Categories/RTMP/VideoSniffer در عمل به موتور وصل نمی‌شدن. */
#define IDM_MAX_PLUGINS  24

typedef struct {
    IDM_PluginHooks *plugins[IDM_MAX_PLUGINS];
    int              count;
} IDM_PluginRegistry;

/* ---- توابع registry ---- */
IDM_PluginRegistry *plugin_registry_create (void);
void                plugin_registry_destroy(IDM_PluginRegistry *reg);
int                 plugin_attach (IDM_PluginRegistry *reg, IDM_PluginHooks *plugin);
void                plugin_detach (IDM_PluginRegistry *reg, const char *name);

/* ---- dispatcher توابع (موتور این‌ها رو صدا می‌زنه) ---- */
void plugin_fire_download_start   (IDM_PluginRegistry *reg, IDM_Download *dl);
void plugin_fire_download_done    (IDM_PluginRegistry *reg, IDM_Download *dl);
void plugin_fire_download_pause   (IDM_PluginRegistry *reg, IDM_Download *dl);
void plugin_fire_download_fail    (IDM_PluginRegistry *reg, IDM_Download *dl);
void plugin_fire_segment_progress (IDM_PluginRegistry *reg, IDM_Segment *seg, uint64_t n);
void plugin_fire_segment_done     (IDM_PluginRegistry *reg, IDM_Segment *seg);
/* برمی‌گردونه: تعداد بایت header اضافه‌شده */
int  plugin_collect_http_headers  (IDM_PluginRegistry *reg, IDM_Download *dl,
                                   char *buf, int max_len);
/* پخش یه Set-Cookie دریافتی به همه‌ی پلاگین‌هایی که on_set_cookie دارن */
void plugin_fire_set_cookie       (IDM_PluginRegistry *reg, IDM_Download *dl,
                                   const char *host, const char *raw_set_cookie);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_API_H */
