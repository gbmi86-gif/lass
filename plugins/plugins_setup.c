/* plugins_setup.c
   ─────────────────────────────────────────────────────────────
   نقطه اتصال پلاگین‌ها به موتور اصلی
   این فایل رو در main.c اضافه کن:
     #include "../plugins/plugins_setup.h"
     ...
     g_registry = plugins_init(g_engine);
     ...
     plugins_shutdown(g_registry);

   کد اصلی (src/) هیچ تغییری نمی‌کنه.
   ─────────────────────────────────────────────────────────────
*/

#include "plugins_setup.h"
#include "plugin_api.h"
#include "../src/lass_log.h"
#include "../src/http.h"
#include "plugin_smart_resume.h"
#include "plugin_dynamic_seg.h"
#include "plugin_cookie_jar.h"
#include "plugin_chmml.h"
#include "plugin_speed_graph.h"
#include "plugin_clipboard.h"
#include "plugin_basket.h"
#include "plugin_ftp.h"
#include "plugin_hls.h"
#include "plugin_proxy.h"
#include "plugin_http_auth.h"
#include "plugin_browser_ext.h"
#include "plugin_dash.h"
#include "plugin_notify.h"
#include "plugin_categories.h"
#include "plugin_rtmp.h"
#include "plugin_video_sniffer.h"
#include <stdio.h>
#include <stdlib.h>

/* ======================================================
   Global registry
   ====================================================== */
static IDM_PluginRegistry *g_registry   = NULL;

/* پلاگین‌های فعال */
static IDM_PluginHooks *g_sr_plugin   = NULL;
static IDM_PluginHooks *g_ds_plugin   = NULL;
static IDM_PluginHooks *g_cj_plugin   = NULL;
static IDM_PluginHooks *g_chmml_plugin = NULL;
static IDM_PluginHooks *g_sg_plugin   = NULL;
static IDM_PluginHooks *g_cb_plugin   = NULL;
static IDM_PluginHooks *g_bk_plugin   = NULL;
static IDM_PluginHooks *g_ftp_plugin  = NULL;
static IDM_PluginHooks *g_hls_plugin  = NULL;
static IDM_PluginHooks *g_proxy_plugin = NULL;
static IDM_PluginHooks *g_auth_plugin  = NULL;
static IDM_PluginHooks *g_ext_plugin   = NULL;
static IDM_PluginHooks *g_dash_plugin  = NULL;
static IDM_PluginHooks *g_notify_plugin = NULL;
static IDM_PluginHooks *g_cat_plugin   = NULL;
static IDM_PluginHooks *g_rtmp_plugin  = NULL;
static IDM_PluginHooks *g_vs_plugin    = NULL;
static IDM_PluginHooks *g_ss_plugin    = NULL;

static VS_FoundCb  g_vs_cb    = NULL;
static void       *g_vs_udata = NULL;

static ExtDownloadCb  g_ext_cb    = NULL;
static void          *g_ext_udata = NULL;

/* notify params */
static HWND  g_notify_tray_hwnd = NULL;
static UINT  g_notify_tray_id   = 0;

/* ini path برای categories */
static char  g_ini_path[512] = {0};

/* پارامترهای speed graph */
static HWND        g_sg_parent = NULL;
static IDM_Engine *g_sg_engine = NULL;

/* پارامتر speed_scheduler */
static IDM_Engine *g_ss_engine = NULL;

void plugins_set_speed_scheduler_engine(IDM_Engine *engine) {
    g_ss_engine = engine;
}

/* پارامترهای clipboard */
static HWND           g_cb_owner   = NULL;
static CB_UrlDetected g_cb_on_url  = NULL;
static void          *g_cb_udata   = NULL;

/* پارامترهای basket */
static HWND           g_bk_parent  = NULL;
static IDM_Engine    *g_bk_engine  = NULL;
static Basket_DropCb  g_bk_on_drop = NULL;
static void          *g_bk_udata   = NULL;

/* ======================================================
   تنظیم پارامترهای speed graph (قبل از plugins_init)
   ====================================================== */
void plugins_set_speed_graph_parent(HWND parent, IDM_Engine *engine) {
    g_sg_parent = parent;
    g_sg_engine = engine;
}

void plugins_set_basket_params(HWND parent, IDM_Engine *engine,
                                Basket_DropCb on_drop, void *userdata) {
    g_bk_parent  = parent;
    g_bk_engine  = engine;
    g_bk_on_drop = on_drop;
    g_bk_udata   = userdata;
}

void plugins_set_clipboard_params(HWND owner, CB_UrlDetected on_url,
                                   void *userdata) {
    g_cb_owner  = owner;
    g_cb_on_url = on_url;
    g_cb_udata  = userdata;
}

void plugins_set_browser_ext_cb(ExtDownloadCb on_download, void *userdata) {
    g_ext_cb    = on_download;
    g_ext_udata = userdata;
}

void plugins_set_notify_params(HWND tray_hwnd, UINT tray_id) {
    g_notify_tray_hwnd = tray_hwnd;
    g_notify_tray_id   = tray_id;
}

void plugins_set_ini_path(const char *ini_path) {
    if (ini_path) strncpy(g_ini_path, ini_path, sizeof(g_ini_path)-1);
}

/* ======================================================
   راه‌اندازی همه پلاگین‌ها
   ====================================================== */
/* wrapper برای ثبت به‌عنوان http.c's header-provider (نگاه کن
   http_set_header_provider) — امضاش باید دقیقاً با
   HttpHeaderProviderFn یکی باشه. */
static int collect_headers_for_http(IDM_Download *dl, char *buf, int max_len) {
    return plugin_collect_http_headers(g_registry, dl, buf, max_len);
}

/* wrapper برای http_set_cookie_handler — نیمه‌ی «دریافت» round-trip
   کوکی که قبلاً کلاً وجود نداشت (رجوع کن به توضیح کامل در
   http.h/plugin_api.h). */
static void dispatch_set_cookie(IDM_Download *dl, const char *host,
                                const char *raw_set_cookie) {
    plugin_fire_set_cookie(g_registry, dl, host, raw_set_cookie);
}

/* wrapper برای http_set_session_customizer — proxy_plugin_apply
   قبلاً تعریف شده بود ولی هیچ‌جا صدا زده نمی‌شد، یعنی تنظیمات
   Proxy که کاربر توی UI ست می‌کرد هیچ‌وقت واقعاً روی دانلود
   اعمال نمی‌شد. */
static void apply_proxy_for_session(HINTERNET hSession) {
    if (g_proxy_plugin) proxy_plugin_apply(g_proxy_plugin, hSession);
}

/* wrapper برای http_set_request_customizer — همین‌طور
   auth_plugin_apply. dl استفاده نمی‌شه چون match فقط بر اساس
   domain هست، نه دانلود خاص؛ امضا برای تطابق با
   HttpRequestCustomizerFn نگه داشته شده. */
static void apply_auth_for_request(IDM_Download *dl, HINTERNET hReq,
                                   const char *host) {
    if (g_auth_plugin) auth_plugin_apply(g_auth_plugin, hReq, host);

    /* ── گفتگو، نه دیکته: If-Range ──
       اگه سرور در HEAD اولیه ETag (یا Last-Modified) داده بود، اون
       رو با هر درخواست بعدی می‌فرستیم. If-Range فقط وقتی معنا داره
       که درخواست Range هم داشته باشه (RFC 7233)، پس روی HEAD بی‌اثره
       و ضرری نداره. معنی‌اش برای ادامه‌ی دانلود (timeout، stall،
       resume): «فقط اگه فایل هنوز همونه بقیه‌اش رو بده؛ اگه عوض شده
       کل فایل رو بفرست.» یعنی سرور خودش تأیید می‌کنه همون فایله و
       بدون دانلود مجدد از صفر ادامه می‌دیم؛ اگه عوض شده باشه، جواب
       200 میاد و segment.c (چک got_partial) به‌جای خراب کردن فایل،
       تمیز fail می‌کنه. ETag اولویت داره؛ validator ضعیف (W/) طبق
       RFC برای If-Range مجاز نیست، پس رد میشه. */
    if (dl) {
        const char *val = NULL;
        if (dl->etag[0] && strncmp(dl->etag, "W/", 2) != 0)
            val = dl->etag;
        else if (dl->last_modified[0])
            val = dl->last_modified;
        if (val) {
            char hdr[256];
            int n = snprintf(hdr, sizeof(hdr), "If-Range: %s\r\n", val);
            if (n > 0 && n < (int)sizeof(hdr))
                HttpAddRequestHeadersA(hReq, hdr, (DWORD)n,
                                       HTTP_ADDREQ_FLAG_ADD |
                                       HTTP_ADDREQ_FLAG_REPLACE);
        }
    }
}

IDM_PluginRegistry *plugins_init(void) {
    g_registry = plugin_registry_create();
    if (!g_registry) return NULL;

#ifdef IDM_DEBUG
    /* ── CHMML memory leak detector (فقط build های Debug) ──
       قبلاً فقط از طریق engine_bridge.c (که هیچ‌وقت از برنامه‌ی
       واقعی صدا زده نمی‌شد) فعال می‌شد — یعنی هیچ‌وقت واقعاً روشن
       نبود. الان این‌جا، جایی که واقعاً پلاگین‌ها init میشن، وصلش
       می‌کنیم. */
    g_chmml_plugin = chmml_plugin_create(true, true);
    if (g_chmml_plugin)
        plugin_attach(g_registry, g_chmml_plugin);
#endif

    /* ── ۱. Smart Resume ── */
    g_sr_plugin = smart_resume_plugin_create(NULL, NULL);
    if (g_sr_plugin)
        plugin_attach(g_registry, g_sr_plugin);

    /* ── ۲. Dynamic Segment ── */
    DynSeg_Config ds_cfg = {
        .split_speed_threshold = 512.0 * 1024.0,  /* 512 KB/s */
        .min_split_size        = 2 * 1024 * 1024,  /* 2 MB */
        .stall_timeout_sec     = 30,
        .max_total_segments    = 16
    };
    g_ds_plugin = dynamic_seg_plugin_create(&ds_cfg);
    if (g_ds_plugin)
        plugin_attach(g_registry, g_ds_plugin);

    /* ── ۳. Cookie Jar ── */
    g_cj_plugin = cookie_jar_plugin_create(NULL);
    if (g_cj_plugin)
        plugin_attach(g_registry, g_cj_plugin);

    /* ── ۴. Speed Graph ── */
    g_sg_plugin = speed_graph_plugin_create(g_sg_parent, g_sg_engine);
    if (g_sg_plugin)
        plugin_attach(g_registry, g_sg_plugin);

    /* ── ۵. Clipboard Monitor ── */
    g_cb_plugin = clipboard_plugin_create(g_cb_owner, g_cb_on_url, g_cb_udata);
    if (g_cb_plugin)
        plugin_attach(g_registry, g_cb_plugin);

    /* ── ۶. Download Basket ── */
    g_bk_plugin = basket_plugin_create(g_bk_parent, g_bk_engine,
                                        g_bk_on_drop, g_bk_udata);
    if (g_bk_plugin)
        plugin_attach(g_registry, g_bk_plugin);

    /* ── ۷. FTP/FTPS ── */
    g_ftp_plugin = ftp_plugin_create();
    if (g_ftp_plugin)
        plugin_attach(g_registry, g_ftp_plugin);

    /* ── ۸. HLS/M3U8 ── */
    g_hls_plugin = hls_plugin_create();
    if (g_hls_plugin)
        plugin_attach(g_registry, g_hls_plugin);

    /* ── ۹. Proxy ── */
    g_proxy_plugin = proxy_plugin_create();
    if (g_proxy_plugin)
        plugin_attach(g_registry, g_proxy_plugin);

    /* ── ۱۰. HTTP Auth ── */
    g_auth_plugin = auth_plugin_create();
    if (g_auth_plugin)
        plugin_attach(g_registry, g_auth_plugin);

    /* ── ۱۱. Browser Extension ── */
    g_ext_plugin = browser_ext_plugin_create(g_ext_cb, g_ext_udata);
    if (g_ext_plugin)
        plugin_attach(g_registry, g_ext_plugin);

    /* ── ۱۲. DASH/MPD ── */
    g_dash_plugin = dash_plugin_create();
    if (g_dash_plugin)
        plugin_attach(g_registry, g_dash_plugin);

    /* ── ۱۳. Notifications ── */
    g_notify_plugin = notify_plugin_create(g_notify_tray_hwnd,
                                            g_notify_tray_id);
    if (g_notify_plugin)
        plugin_attach(g_registry, g_notify_plugin);

    /* ── ۱۴. Categories ── */
    g_cat_plugin = categories_plugin_create(
        g_ini_path[0] ? g_ini_path : NULL);
    if (g_cat_plugin)
        plugin_attach(g_registry, g_cat_plugin);

    /* ── ۱۵. RTMP ── */
    g_rtmp_plugin = rtmp_plugin_create();
    if (g_rtmp_plugin)
        plugin_attach(g_registry, g_rtmp_plugin);

    /* ── ۱۶. Video Sniffer ── */
    g_vs_plugin = vs_plugin_create(g_vs_cb, g_vs_udata);
    if (g_vs_plugin)
        plugin_attach(g_registry, g_vs_plugin);

    /* ── Speed Scheduler ──
       فقط اگه engine قبلاً ست شده باشه می‌سازیم چون این پلاگین
       نیاز به engine_set_global_speed_limit داره */
    if (g_ss_engine) {
        g_ss_plugin = speed_sched_plugin_create(g_ss_engine);
        if (g_ss_plugin)
            plugin_attach(g_registry, g_ss_plugin);
    }

    LOGI(LOG_MOD_PLUGINS, "%d plugin(s) loaded", g_registry->count);

    /* ── وصل کردن Cookie Jar (و هر پلاگین دیگه‌ای که در آینده
       on_http_headers ثبت کنه) به http.c ──
       plugin_collect_http_headers از قبل تعریف شده بود ولی هیچ‌جا
       صدا زده نمی‌شد — یعنی Cookie Jar هیچ‌وقت واقعاً کوکی نمی‌فرستاد.
       src/http.c از plugins/ چیزی نمی‌دونه، پس فقط یه تابع کوچیک
       این‌جا ثبت می‌کنیم که در لحظه‌ی درخواست صدا زده میشه. */
    http_set_header_provider(collect_headers_for_http);

    /* نیمه‌ی «دریافت» round-trip کوکی — تا امروز اصلاً وجود
       نداشت؛ cookie_jar فقط می‌تونست کوکی‌های از پیش import‌شده
       رو بفرسته، نه اینکه یه session cookie تازه از خود سرور
       واقعاً یاد بگیره. */
    http_set_cookie_handler(dispatch_set_cookie);

    /* همون شکاف برای Proxy و HTTP Auth — این دو تابع (که از قبل
       در plugin_proxy.c/plugin_http_auth.c پیاده‌سازی شده بودن)
       تا امروز هیچ‌جا صدا زده نمی‌شدن. */
    http_set_session_customizer(apply_proxy_for_session);
    http_set_request_customizer(apply_auth_for_request);

    return g_registry;
}

/* ======================================================
   سیگنال زودهنگام توقف — رجوع کن به توضیح کامل در plugins_setup.h
   و main.c. فقط پلاگین‌هایی که یه thread پس‌زمینه‌ی دائمی (loop-
   based، نه per-task) دارن نیاز به این دارن: dynamic_seg،
   speed_graph، speed_scheduler، browser_ext. بقیه‌ی پلاگین‌ها یا
   اصلاً thread ندارن، یا thread‌هاشون per-task/on-demand هستن
   (rtmp_worker، video_sniffer_worker) که چیزی برای signal-کردن
   زودهنگام ندارن. */
void plugins_signal_shutdown(IDM_PluginRegistry *reg) {
    (void)reg; /* هر ۴ پلاگین از globalهای همین فایل استفاده می‌کنن */
    if (g_ds_plugin)  dynamic_seg_plugin_signal_stop(g_ds_plugin);
    if (g_sg_plugin)  speed_graph_plugin_signal_stop(g_sg_plugin);
    if (g_ss_plugin)  speed_sched_plugin_signal_stop(g_ss_plugin);
    if (g_ext_plugin) browser_ext_plugin_signal_stop(g_ext_plugin);
}

/* ======================================================
   خاموش کردن همه پلاگین‌ها
   ====================================================== */
void plugins_shutdown(IDM_PluginRegistry *reg) {
    if (!reg) return;

    /* callback های http.c رو *قبل* از آزاد کردن registry جدا کن.
       قبلاً بعد از shutdown هنوز به g_registry (آزادشده) اشاره
       می‌کردن؛ هر thread ای که هنوز داشت درخواست HTTP می‌زد
       (مثلاً یه segment zombie) از حافظه‌ی آزادشده می‌خوند. */
    http_set_header_provider(NULL);
    http_set_cookie_handler(NULL);
    http_set_request_customizer(NULL);
    http_set_session_customizer(NULL);

    plugin_registry_destroy(reg);
    if (reg == g_registry) g_registry = NULL;

    if (g_sr_plugin)    { smart_resume_plugin_destroy(g_sr_plugin); g_sr_plugin   = NULL; }
    if (g_ds_plugin)    { dynamic_seg_plugin_destroy(g_ds_plugin);  g_ds_plugin   = NULL; }
    if (g_cj_plugin)    { cookie_jar_plugin_destroy(g_cj_plugin);   g_cj_plugin   = NULL; }
    if (g_sg_plugin)    { speed_graph_plugin_destroy(g_sg_plugin);  g_sg_plugin   = NULL; }
    if (g_cb_plugin)    { clipboard_plugin_destroy(g_cb_plugin);    g_cb_plugin   = NULL; }
    if (g_bk_plugin)    { basket_plugin_destroy(g_bk_plugin);       g_bk_plugin   = NULL; }
    if (g_ftp_plugin)   { ftp_plugin_destroy(g_ftp_plugin);         g_ftp_plugin   = NULL; }
    if (g_hls_plugin)   { hls_plugin_destroy(g_hls_plugin);         g_hls_plugin   = NULL; }
    if (g_proxy_plugin) { proxy_plugin_destroy(g_proxy_plugin);      g_proxy_plugin = NULL; }
    if (g_auth_plugin)  { auth_plugin_destroy(g_auth_plugin);        g_auth_plugin  = NULL; }
    if (g_ext_plugin)   { browser_ext_plugin_destroy(g_ext_plugin);  g_ext_plugin   = NULL; }
    if (g_dash_plugin)  { dash_plugin_destroy(g_dash_plugin);         g_dash_plugin  = NULL; }
    if (g_notify_plugin){ notify_plugin_destroy(g_notify_plugin);     g_notify_plugin= NULL; }
    if (g_cat_plugin)   { categories_plugin_destroy(g_cat_plugin);    g_cat_plugin   = NULL; }
    if (g_rtmp_plugin)  { rtmp_plugin_destroy(g_rtmp_plugin);          g_rtmp_plugin  = NULL; }
    if (g_vs_plugin)    { vs_plugin_destroy(g_vs_plugin);               g_vs_plugin    = NULL; }
    if (g_ss_plugin)    { speed_sched_plugin_destroy(g_ss_plugin);      g_ss_plugin    = NULL; }
    if (g_chmml_plugin) { chmml_plugin_destroy(g_chmml_plugin);     g_chmml_plugin = NULL; }
}

/* ======================================================
   Dispatcher‌های کوتاه برای استفاده در engine.c
   بدون لمس کد اصلی:
   فقط engine.c باید این توابع رو صدا بزنه
   ====================================================== */
void plugins_on_download_start(IDM_Download *dl) {
    plugin_fire_download_start(g_registry, dl);
}
void plugins_on_download_done(IDM_Download *dl) {
    plugin_fire_download_done(g_registry, dl);
}
void plugins_on_download_pause(IDM_Download *dl) {
    plugin_fire_download_pause(g_registry, dl);
}
void plugins_on_download_fail(IDM_Download *dl) {
    plugin_fire_download_fail(g_registry, dl);
}
void plugins_on_segment_progress(IDM_Segment *seg, uint64_t n) {
    plugin_fire_segment_progress(g_registry, seg, n);
}
void plugins_collect_http_headers(IDM_Download *dl, char *buf, int max_len) {
    plugin_collect_http_headers(g_registry, dl, buf, max_len);
}

/* ======================================================
   دسترسی مستقیم به پلاگین‌ها (برای GUI)
   ====================================================== */
IDM_PluginHooks *plugins_get_cookie_jar  (void) { return g_cj_plugin; }
IDM_PluginHooks *plugins_get_smart_resume(void) { return g_sr_plugin; }
IDM_PluginHooks *plugins_get_dynamic_seg (void) { return g_ds_plugin; }
IDM_PluginHooks *plugins_get_chmml       (void) { return g_chmml_plugin; }
IDM_PluginHooks *plugins_get_speed_graph (void) { return g_sg_plugin; }
IDM_PluginHooks *plugins_get_clipboard   (void) { return g_cb_plugin; }
IDM_PluginHooks *plugins_get_basket      (void) { return g_bk_plugin; }
IDM_PluginHooks *plugins_get_ftp         (void) { return g_ftp_plugin; }
IDM_PluginHooks *plugins_get_hls         (void) { return g_hls_plugin; }
IDM_PluginHooks *plugins_get_proxy       (void) { return g_proxy_plugin; }
IDM_PluginHooks *plugins_get_auth        (void) { return g_auth_plugin; }
IDM_PluginHooks *plugins_get_browser_ext (void) { return g_ext_plugin; }
IDM_PluginHooks *plugins_get_dash        (void) { return g_dash_plugin; }
IDM_PluginHooks *plugins_get_notify      (void) { return g_notify_plugin; }
IDM_PluginHooks *plugins_get_categories  (void) { return g_cat_plugin; }
IDM_PluginHooks *plugins_get_rtmp        (void) { return g_rtmp_plugin; }
IDM_PluginHooks *plugins_get_video_sniffer(void) { return g_vs_plugin; }
IDM_PluginHooks *plugins_get_speed_scheduler(void) { return g_ss_plugin; }

void plugins_set_video_sniffer_cb(VS_FoundCb on_found, void *userdata) {
    g_vs_cb    = on_found;
    g_vs_udata = userdata;
}
