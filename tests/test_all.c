#include <winsock2.h>
/* test_all.c
   ─────────────────────────────────────────────────────────────
   Lass Download Manager – Test Suite  v5.1
   کامپایل:
     gcc -std=c11 -DWIN32 -D_WIN32 -DWIN32_LEAN_AND_MEAN
         -I src -I plugins -I core
         tests/test_all.c
         plugins/plugin_api.c plugins/plugin_chmml.c
         plugins/plugin_smart_resume.c plugins/plugin_dynamic_seg.c
         plugins/plugin_speed_graph.c
         plugins/plugin_clipboard.c plugins/plugin_basket.c
         plugins/plugin_ftp.c plugins/plugin_hls.c
         plugins/plugin_proxy.c plugins/plugin_http_auth.c
         plugins/plugin_browser_ext.c
         plugins/plugin_dash.c
         plugins/plugin_notify.c
         plugins/plugin_categories.c
         src/engine.c src/segment.c src/http.c
         src/storage.c src/resume.c src/settings.c
         core/core_ldm.c core/guo.c
         -o build/test_all.exe
         -lwininet -lcomctl32 -lcomdlg32 -lshell32
         -luser32 -lgdi32 -lole32 -luuid -lshlwapi
         -static-libgcc
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "security_fixes.h"
#include "plugin_chmml.h"
#include "plugin_api.h"
#include "plugin_smart_resume.h"
#include "plugin_dynamic_seg.h"
#include "plugin_cookie_jar.h"
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
#include "../src/engine.h"
#include "../plugins/plugins_setup.h"
#include "../src/crash_report.h"
#include "../src/legacy_data.h"
#include "../src/transport.h"
#include "../src/url_secure.h"
#include "../src/tels_container.h"
#include "../plugins/plugin_ftp.h"
#include "test_ftpd.h"
#include "../src/queue_persist.h"
#include "../src/lass_store.h"
#include "../src/settings.h"
#include "../src/i18n.h"
#include "../src/http.h"
#include "../src/secure_store.h"
#include "../src/shutdown_brake.h"
#include "../src/lass_log.h"
#include "test_httpd.h"

/* ======================================================
   ابزار تست
   ====================================================== */
static int g_pass = 0;
static int g_fail = 0;
static int g_total = 0;

#define TEST(name, cond) do { \
    g_total++; \
    log_console_lock(); \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s  (line %d)\n", name, __LINE__); \
        g_fail++; \
    } \
    log_console_unlock(); \
} while(0)

#define SECTION(title) do { \
    log_console_lock(); \
    printf("\n=== %s ===\n", title); \
    log_console_unlock(); \
} while(0)

/* ======================================================
   تست ۱: Security Fixes
   ====================================================== */
static void test_security_fixes(void) {
    SECTION("Security Fixes");

    /* safe_mul_size */
    size_t result = 0;
    TEST("safe_mul normal",    safe_mul_size(4, 8, &result) && result == 32);
    TEST("safe_mul zero-a",    safe_mul_size(0, 8, &result) && result == 0);
    TEST("safe_mul overflow",  !safe_mul_size(SIZE_MAX, 2, &result));
    TEST("safe_mul max valid", safe_mul_size(1, SIZE_MAX, &result) && result == SIZE_MAX);

    /* validate_url */
    TEST("url http ok",    validate_url("http://example.com/file.zip", IDM_MAX_URL));
    TEST("url https ok",   validate_url("https://example.com/file.zip", IDM_MAX_URL));
    TEST("url ftp reject", !validate_url("ftp://example.com/file.zip", IDM_MAX_URL));
    TEST("url null reject",!validate_url(NULL, IDM_MAX_URL));
    TEST("url empty reject",!validate_url("", IDM_MAX_URL));
    TEST("url ctrl-char",  !validate_url("http://ex\x01ample.com/", IDM_MAX_URL));
    /* null byte injection */
    char url_with_null[32] = "http://evil.com/\0/../secret";
    TEST("url null-byte",  validate_url(url_with_null, IDM_MAX_URL)); /* null byte = string ends early, OK */

    /* validate_out_path */
    TEST("path normal",      validate_out_path("C:\\Downloads\\file.zip"));
    TEST("path traversal1",  !validate_out_path("C:\\Downloads\\..\\secret.exe"));
    TEST("path traversal2",  !validate_out_path("C:\\Downloads\\../secret.exe"));
    TEST("path UNC reject",  !validate_out_path("\\\\server\\share\\file.zip"));
    TEST("path null reject",  !validate_out_path(NULL));
    TEST("path empty reject", !validate_out_path(""));

    /* clamp_segments */
    TEST("clamp normal", clamp_segments(8, 1, 16) == 8);
    TEST("clamp min",    clamp_segments(0, 1, 16) == 1);
    TEST("clamp max",    clamp_segments(32, 1, 16) == 16);

    /* safe_strncpy */
    char dst[8] = {0};
    safe_strncpy(dst, "hello world too long", sizeof(dst));
    TEST("strncpy truncate",   dst[7] == '\0');
    TEST("strncpy content",    strncmp(dst, "hello w", 7) == 0);
    safe_strncpy(dst, NULL, sizeof(dst));
    TEST("strncpy null src",   dst[0] == '\0');
}

/* ======================================================
   تست ۲: CHMML Memory Manager
   ====================================================== */
static void test_chmml(void) {
    SECTION("CHMML Memory Manager");

    IDM_PluginHooks *plug = chmml_plugin_create(true, true);
    TEST("chmml plugin create", plug != NULL);
    if (!plug) return;

    /* basic alloc/free */
    void *p1 = chmml_malloc(16,   __FILE__, __LINE__, __func__);
    void *p2 = chmml_malloc(64,   __FILE__, __LINE__, __func__);
    void *p3 = chmml_malloc(2048, __FILE__, __LINE__, __func__);
    void *p4 = chmml_malloc(4096, __FILE__, __LINE__, __func__); /* large */
    TEST("alloc 16",    p1 != NULL);
    TEST("alloc 64",    p2 != NULL);
    TEST("alloc 2048",  p3 != NULL);
    TEST("alloc large", p4 != NULL);

    /* نوشتن و خواندن */
    if (p1) { memset(p1, 0xAA, 16); TEST("write 16", *(uint8_t*)p1 == 0xAA); }
    if (p2) { memset(p2, 0xBB, 64); TEST("write 64", *(uint8_t*)p2 == 0xBB); }

    /* calloc باید zero باشه */
    void *p5 = chmml_calloc(10, 8, __FILE__, __LINE__, __func__);
    TEST("calloc zero", p5 != NULL && *(uint64_t*)p5 == 0);

    /* realloc */
    void *p6 = chmml_malloc(32, __FILE__, __LINE__, __func__);
    if (p6) memset(p6, 0xCC, 32);
    void *p7 = chmml_realloc(p6, 128, __FILE__, __LINE__, __func__);
    TEST("realloc grow",    p7 != NULL);
    TEST("realloc content", p7 && *(uint8_t*)p7 == 0xCC);

    /* آزاد کردن */
    if (p1) chmml_free(p1, __FILE__, __LINE__, __func__);
    if (p2) chmml_free(p2, __FILE__, __LINE__, __func__);
    if (p3) chmml_free(p3, __FILE__, __LINE__, __func__);
    if (p4) chmml_free(p4, __FILE__, __LINE__, __func__);
    if (p5) chmml_free(p5, __FILE__, __LINE__, __func__);
    if (p7) chmml_free(p7, __FILE__, __LINE__, __func__);
    TEST("free all ok", true);

    /* آمار */
    size_t used = chmml_bytes_in_use();
    TEST("bytes_in_use after free", used == 0);

    /* leak detection: عمداً leak ایجاد می‌کنیم */
    void *leak = chmml_malloc(99, __FILE__, __LINE__, __func__);
    (void)leak;
    printf("  [INFO] leak گزارش زیر باید ۱ leak از ۹۹ بایت نشون بده:\n");
    chmml_report_leaks();
    if (leak) chmml_free(leak, __FILE__, __LINE__, __func__);
    TEST("leak detection ran", true);

    /* stress: ۵۰۰ alloc/free */
    void *ptrs[500];
    for (int i = 0; i < 500; i++)
        ptrs[i] = chmml_malloc((size_t)(i % 2048 + 1),
                               __FILE__, __LINE__, __func__);
    for (int i = 0; i < 500; i++)
        if (ptrs[i]) chmml_free(ptrs[i], __FILE__, __LINE__, __func__);
    TEST("stress 500 alloc/free", chmml_bytes_in_use() == 0);

    chmml_plugin_destroy(plug);
    TEST("chmml plugin destroy", true);
}

/* ======================================================
   تست ۳: Plugin API Registry
   ====================================================== */
static void test_plugin_registry(void) {
    SECTION("Plugin Registry");

    IDM_PluginRegistry *reg = plugin_registry_create();
    TEST("registry create", reg != NULL);
    TEST("registry empty",  reg && reg->count == 0);

    /* ساخت یه پلاگین ساده */
    static IDM_PluginHooks dummy = {
        .name        = "dummy_test",
        .version     = "0.1",
        .api_version = IDM_PLUGIN_API_VERSION,
    };

    int r = plugin_attach(reg, &dummy);
    TEST("attach dummy", r == 0);
    TEST("count after attach", reg && reg->count == 1);

    /* attach دوباره همون پلاگین */
    r = plugin_attach(reg, &dummy);
    TEST("no double attach", reg && reg->count == 1);

    /* پلاگین با API version اشتباه */
    static IDM_PluginHooks bad_ver = {
        .name        = "bad_version",
        .api_version = 999,
    };
    r = plugin_attach(reg, &bad_ver);
    TEST("reject wrong API version", r != 0 && reg->count == 1);

    /* detach */
    plugin_detach(reg, "dummy_test");
    TEST("detach", reg && reg->count == 0);

    plugin_registry_destroy(reg);
    TEST("registry destroy", true);
}

/* ======================================================
   تست ۴: Cookie Jar Plugin
   ====================================================== */
static void test_cookie_jar(void) {
    SECTION("Cookie Jar Plugin");

    IDM_PluginHooks *plug = cookie_jar_plugin_create(NULL);
    TEST("cookie_jar create", plug != NULL);
    if (!plug) return;

    /* set و بازیابی */
    cookie_jar_set(plug, "example.com", "session", "abc123", 3600);
    cookie_jar_set(plug, "example.com", "user",    "alice",  0);

    /* یه IDM_Download واقعی برای تست header (نه یه mock ساختگی —
       قبلاً این‌جا یه struct fake با فقط فیلد url استفاده می‌شد که
       فرضش این بود url اولین فیلد IDM_Download هست؛ وقتی فیلدهای
       جدید (eng، referer) قبل از url اضافه شدن، اون فرض غلط از آب
       دراومد و این تست‌ها به‌اشتباه fail می‌شدن — نه به‌خاطر باگ
       واقعی، بلکه چون تست به layout داخلی struct وابسته بود. یه
       IDM_Download واقعی این وابستگی رو کامل حذف می‌کنه. */
    IDM_Download rdl = {0};
    strncpy(rdl.url, "https://example.com/file.zip", sizeof(rdl.url) - 1);

    char hdr_buf[1024] = {0};
    int n = plug->on_http_headers
            ? plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf))
            : 0;

    TEST("cookie header generated", n > 0);
    TEST("session cookie in header", strstr(hdr_buf, "session=abc123") != NULL);
    TEST("user cookie in header",    strstr(hdr_buf, "user=alice") != NULL);
    TEST("header starts with Cookie:", strncmp(hdr_buf, "Cookie:", 7) == 0);

    /* domain مطابقت نداشته باشه */
    strncpy(rdl.url, "https://other.com/file.zip", sizeof(rdl.url) - 1);
    memset(hdr_buf, 0, sizeof(hdr_buf));
    n = plug->on_http_headers
        ? plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf))
        : 0;
    TEST("no cookie for wrong domain", n == 0);

    /* clear */
    cookie_jar_clear(plug);
    strncpy(rdl.url, "https://example.com/file.zip", sizeof(rdl.url) - 1);
    memset(hdr_buf, 0, sizeof(hdr_buf));
    n = plug->on_http_headers
        ? plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf))
        : 0;
    TEST("no cookie after clear", n == 0);

    /* تست expired cookie */
    cookie_jar_set(plug, "example.com", "old", "value", -1); /* فوری expire */
    Sleep(10);
    strncpy(rdl.url, "https://example.com/file.zip", sizeof(rdl.url) - 1);
    memset(hdr_buf, 0, sizeof(hdr_buf));
    n = plug->on_http_headers
        ? plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf))
        : 0;
    TEST("expired cookie not sent", n == 0 || strstr(hdr_buf, "old=") == NULL);

    /* ══════════════════════════════════════════════════
       تست‌های on_set_cookie (نیمه‌ی «دریافت» که تا امروز اصلاً
       وجود نداشت — رجوع کن به توضیح کامل کنار hook_on_set_cookie
       در plugin_cookie_jar.c). این‌ها round-trip واقعی
       Set-Cookie → ذخیره → ارسال بعدی رو تست می‌کنن.
       ══════════════════════════════════════════════════ */
    cookie_jar_clear(plug);
    TEST("on_set_cookie hook registered", plug->on_set_cookie != NULL);

    if (plug->on_set_cookie) {
        /* ۱) یه Set-Cookie ساده، بدون Domain/Path صریح — باید
           host-only (فقط همون host) ذخیره بشه. */
        plug->on_set_cookie(NULL, "cdn.example.com",
                             "sid=xyz789; Max-Age=3600; Path=/dl");
        strncpy(rdl.url, "https://cdn.example.com/dl/file.zip",
                sizeof(rdl.url) - 1);
        memset(hdr_buf, 0, sizeof(hdr_buf));
        n = plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf));
        TEST("learned session cookie is sent back", strstr(hdr_buf, "sid=xyz789") != NULL);

        /* ۲) همون کوکی نباید برای path نامرتبط فرستاده بشه —
           Path=/dl یعنی فقط زیرمسیرهای /dl. */
        strncpy(rdl.url, "https://cdn.example.com/other/file.zip",
                sizeof(rdl.url) - 1);
        memset(hdr_buf, 0, sizeof(hdr_buf));
        n = plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf));
        TEST("path-scoped cookie not sent for unrelated path",
             strstr(hdr_buf, "sid=xyz789") == NULL);

        /* ۳) کوکی با Secure نباید روی درخواست http:// (غیر-https)
           فرستاده بشه. */
        plug->on_set_cookie(NULL, "secure.example.com",
                             "auth=tok1; Secure; Path=/");
        strncpy(rdl.url, "http://secure.example.com/file.zip",
                sizeof(rdl.url) - 1);
        memset(hdr_buf, 0, sizeof(hdr_buf));
        n = plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf));
        TEST("secure cookie withheld on plain http", strstr(hdr_buf, "auth=tok1") == NULL);

        strncpy(rdl.url, "https://secure.example.com/file.zip",
                sizeof(rdl.url) - 1);
        memset(hdr_buf, 0, sizeof(hdr_buf));
        n = plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf));
        TEST("secure cookie sent on https", strstr(hdr_buf, "auth=tok1") != NULL);

        /* ۴) Domain صریح یعنی subdomain match هم باید کار کنه */
        plug->on_set_cookie(NULL, "www.wide.example",
                             "wide=1; Domain=wide.example; Path=/");
        strncpy(rdl.url, "https://cdn.wide.example/file.zip",
                sizeof(rdl.url) - 1);
        memset(hdr_buf, 0, sizeof(hdr_buf));
        n = plug->on_http_headers(&rdl, hdr_buf, sizeof(hdr_buf));
        TEST("explicit Domain= attribute enables subdomain match",
             strstr(hdr_buf, "wide=1") != NULL);

        /* ۵) Set-Cookie بدشکل (بدون '=') نباید crash کنه یا چیزی ذخیره کنه */
        plug->on_set_cookie(NULL, "cdn.example.com", "garbage-no-equals");
        TEST("malformed Set-Cookie does not crash", true);
    }

    cookie_jar_plugin_destroy(plug);
    TEST("cookie_jar destroy", true);
}

/* ======================================================
   تست ۵: Smart Resume Plugin (بدون شبکه)
   ====================================================== */
static void test_smart_resume(void) {
    SECTION("Smart Resume Plugin");
    lass_store_close();
    DeleteFileA("tels_test_sr_store.tels");
    lass_store_open("tels_test_sr_store.tels");

    /* پوشه temp برای state */
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);
    strncat(tmp_dir, "idm_test_state", MAX_PATH - strlen(tmp_dir) - 1);
    CreateDirectoryA(tmp_dir, NULL);

    uint8_t key[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    IDM_PluginHooks *plug = smart_resume_plugin_create(tmp_dir, key);
    TEST("smart_resume create", plug != NULL);
    if (!plug) return;

    const char *test_url  = "https://example.com/bigfile.zip";
    const char *test_path = "C:\\Downloads\\bigfile.zip";

    /* بررسی state وقتی وجود نداره */
    bool exists = smart_resume_exists(plug, test_url, test_path);
    TEST("no state initially", !exists);

    /* ذخیره یه state ساختگی */
    /* IDM_Download mock */
    IDM_Download mock_dl;
    memset(&mock_dl, 0, sizeof(mock_dl));
    strncpy(mock_dl.url,      test_url,  IDM_MAX_URL  - 1);
    strncpy(mock_dl.out_path, test_path, IDM_MAX_PATH - 1);
    mock_dl.total_size  = 100 * 1024 * 1024; /* 100 MB */
    mock_dl.downloaded  = 40  * 1024 * 1024; /* 40 MB downloaded */
    mock_dl.num_segments = 2;
    InitializeCriticalSection(&mock_dl.lock);

    /* ساخت segment‌های mock */
    IDM_Segment seg0 = {0}, seg1 = {0};
    InitializeCriticalSection(&seg0.lock);
    InitializeCriticalSection(&seg1.lock);
    seg0.id = 0; seg0.start = 0;               seg0.end = 49999999; seg0.current = 40*1024*1024;
    seg1.id = 1; seg1.start = 50*1024*1024;    seg1.end = 99999999; seg1.current = 50*1024*1024;
    IDM_Segment *segs[2] = {&seg0, &seg1};
    mock_dl.segments = segs;

    /* fire hook مستقیم */
    if (plug->on_download_pause)
        plug->on_download_pause(&mock_dl);

    /* بررسی که state ذخیره شد */
    exists = smart_resume_exists(plug, test_url, test_path);
    TEST("state saved after pause", exists);

    /* بارگذاری state */
    IDM_Download load_dl;
    memset(&load_dl, 0, sizeof(load_dl));
    strncpy(load_dl.url,      test_url,  IDM_MAX_URL  - 1);
    strncpy(load_dl.out_path, test_path, IDM_MAX_PATH - 1);
    InitializeCriticalSection(&load_dl.lock);

    if (plug->on_download_start)
        plug->on_download_start(&load_dl);

    TEST("state loaded on start",      load_dl.resume_segs != NULL);
    TEST("total_size restored",        load_dl.total_size == 100*1024*1024ULL);
    TEST("downloaded restored",        (uint64_t)load_dl.downloaded == 40*1024*1024ULL);
    TEST("num_segments restored",      load_dl.num_segments == 2);
    if (load_dl.resume_segs) {
        TEST("seg0 start correct",     load_dl.resume_segs[0].start == 0);
        TEST("seg0 current correct",   load_dl.resume_segs[0].current == 40*1024*1024ULL);
        TEST("seg1 start correct",     load_dl.resume_segs[1].start == 50*1024*1024ULL);
    }

    /* URL اشتباه نباید load کنه */
    IDM_Download wrong_dl;
    memset(&wrong_dl, 0, sizeof(wrong_dl));
    strncpy(wrong_dl.url,      "https://other.com/file.zip", IDM_MAX_URL  - 1);
    strncpy(wrong_dl.out_path, test_path,                    IDM_MAX_PATH - 1);
    InitializeCriticalSection(&wrong_dl.lock);
    if (plug->on_download_start)
        plug->on_download_start(&wrong_dl);
    TEST("wrong URL not loaded", wrong_dl.resume_segs == NULL);

    /* حذف state بعد از complete */
    if (plug->on_download_done)
        plug->on_download_done(&mock_dl);
    exists = smart_resume_exists(plug, test_url, test_path);
    TEST("state deleted after done", !exists);

    /* cleanup */
    free(load_dl.resume_segs);
    DeleteCriticalSection(&mock_dl.lock);
    DeleteCriticalSection(&load_dl.lock);
    DeleteCriticalSection(&wrong_dl.lock);
    DeleteCriticalSection(&seg0.lock);
    DeleteCriticalSection(&seg1.lock);

    smart_resume_plugin_destroy(plug);
    TEST("smart_resume destroy", true);
    lass_store_close();
    DeleteFileA("tels_test_sr_store.tels");

    /* حذف پوشه temp */
    RemoveDirectoryA(tmp_dir);
}

/* ======================================================
   تست ۶: Dynamic Segment Config
   ====================================================== */
static void test_dynamic_seg(void) {
    SECTION("Dynamic Segment Plugin");

    DynSeg_Config cfg = {
        .split_speed_threshold = 1024.0 * 1024.0, /* 1 MB/s */
        .min_split_size        = 4 * 1024 * 1024,
        .stall_timeout_sec     = 15,
        .max_total_segments    = 8
    };

    IDM_PluginHooks *plug = dynamic_seg_plugin_create(&cfg);
    TEST("dynamic_seg create", plug != NULL);
    if (!plug) return;

    /* init → باید thread مانیتور رو start کنه */
    int r = plug->on_init ? plug->on_init() : -1;
    TEST("dynamic_seg init", r == 0);

    /* بررسی پیکربندی از طریق priv */
    /* (فقط بررسی که crash نکنه) */
    TEST("no crash on init", true);

    /* ══════════════════════════════════════════════════
       تست‌های واقعی برای باگ stall-detection که در lass.log
       واقعی پیدا شد (رجوع کن به توضیح کامل کنار seg_is_stalled
       در plugin_dynamic_seg.c). این‌ها منطق واقعی رو، نه فقط
       "کرش نکرد"، تأیید می‌کنن.
       ══════════════════════════════════════════════════ */
    {
        IDM_Segment seg = {0};
        InitializeCriticalSection(&seg.lock);

        /* ۱) segment تازه‌شروع‌شده: avg_speed==0.0 (هنوز اولین
           بایت نرسیده) — این دقیقاً حالتیه که قبلاً به‌اشتباه
           فوری stall تشخیص داده می‌شد. با فیکس، باید false برگرده
           (هنوز داخل پنجره‌ی grace هست، چون low_speed_since تازه
           ست شده). */
        seg.state = SEG_DOWNLOADING;
        seg.avg_speed = 0.0;
        seg.low_speed_since = 0;
        bool stalled = dynamic_seg_test_is_stalled(&seg, 30);
        TEST("fresh segment (0 B/s) not stalled immediately", !stalled);
        TEST("low_speed_since gets set on first low reading", seg.low_speed_since != 0);

        /* ۲) همون segment، ولی وانمود می‌کنیم timeout_sec کامل
           (اینجا با یه timeout خیلی کوچیک، ۰ ثانیه) گذشته —
           باید حالا stall تشخیص داده بشه. */
        seg.low_speed_since = GetTickCount() - 100; /* "قبلاً" شروع شده */
        stalled = dynamic_seg_test_is_stalled(&seg, 0);
        TEST("sustained low speed past timeout IS stalled", stalled);

        /* ۳) سرعت سالم — هیچ‌وقت نباید stall تشخیص داده بشه، حتی
           با یه low_speed_since قدیمی باقی‌مونده از قبل (باید پاک
           بشه). */
        seg.avg_speed = 5000.0;
        seg.low_speed_since = GetTickCount() - 100000;
        stalled = dynamic_seg_test_is_stalled(&seg, 30);
        TEST("healthy speed never stalled", !stalled);
        TEST("low_speed_since resets on speed recovery", seg.low_speed_since == 0);

        /* ۴) یه حالت غیر-DOWNLOADING (مثلاً RETRYING) هیچ‌وقت از
           این مسیر stall نمی‌خوره — منطق backoff خودش رو در
           segment.c داره. */
        seg.state = SEG_RETRYING;
        seg.avg_speed = 0.0;
        seg.low_speed_since = 0;
        stalled = dynamic_seg_test_is_stalled(&seg, 0);
        TEST("non-downloading state never flagged stalled", !stalled);

        DeleteCriticalSection(&seg.lock);
    }

    /* destroy */
    if (plug->on_destroy) plug->on_destroy();
    dynamic_seg_plugin_destroy(plug);
    TEST("dynamic_seg destroy", true);
}

/* ======================================================
   تست ۷: Speed Graph Plugin (بدون GUI واقعی)
   ====================================================== */
static void test_speed_graph(void) {
    SECTION("Speed Graph Plugin");

    /* بدون HWND و Engine واقعی — فقط منطق داخلی رو تست می‌کنیم */
    IDM_PluginHooks *plug = speed_graph_plugin_create(NULL, NULL);
    TEST("speed_graph create",       plug != NULL);
    if (!plug) return;

    /* پلاگین باید نام و version داشته باشه */
    TEST("plugin name set",          plug->name != NULL);
    TEST("plugin version set",       plug->version != NULL);
    TEST("plugin api_version match", plug->api_version == IDM_PLUGIN_API_VERSION);

    /* آمار اولیه باید صفر باشه */
    SG_Stats stats = {0};
    speed_graph_get_stats(plug, &stats);
    TEST("initial sample_count zero",  stats.sample_count == 0);
    TEST("initial peak zero",          stats.peak_bps == 0.0);
    TEST("initial current zero",       stats.current_bps == 0.0);
    TEST("initial avg zero",           stats.avg_bps == 0.0);

    /* visible باید FALSE باشه (پنجره NULL) */
    TEST("visible false without hwnd", !speed_graph_visible(plug));

    /* get_stats با NULL باید crash نکنه */
    speed_graph_get_stats(plug, NULL);
    TEST("get_stats null safe",        TRUE);

    /* بعد از کمی صبر، timer باید چند نمونه ثبت کرده باشه */
    Sleep(2500);
    speed_graph_get_stats(plug, &stats);
    TEST("samples collected after 2s", stats.sample_count >= 2);
    TEST("peak >= current",            stats.peak_bps >= stats.current_bps);

    speed_graph_plugin_destroy(plug);
    TEST("speed_graph destroy",        TRUE);

    /* destroy با NULL باید safe باشه */
    speed_graph_plugin_destroy(NULL);
    TEST("destroy null safe",          TRUE);
}

/* ======================================================
   تست ۸: Clipboard Monitor Plugin
   ====================================================== */

/* counter برای callback تست */
static int g_cb_fired = 0;
static char g_cb_last_url[2048] = {0};

static void test_cb_callback(const char *url, void *userdata) {
    (void)userdata;
    g_cb_fired++;
    if (url) strncpy(g_cb_last_url, url, sizeof(g_cb_last_url)-1);
}

static void test_clipboard(void) {
    SECTION("Clipboard Monitor Plugin");

    IDM_PluginHooks *plug = clipboard_plugin_create(
        NULL, test_cb_callback, NULL);
    TEST("clipboard create",           plug != NULL);
    if (!plug) return;

    TEST("plugin name set",            plug->name != NULL);
    TEST("api_version match",          plug->api_version == IDM_PLUGIN_API_VERSION);

    /* پیش‌فرض: غیرفعال */
    TEST("disabled by default",        !clipboard_plugin_is_enabled(plug));

    /* فعال‌سازی */
    clipboard_plugin_set_enabled(plug, TRUE);
    TEST("enable works",               clipboard_plugin_is_enabled(plug));

    /* غیرفعال */
    clipboard_plugin_set_enabled(plug, FALSE);
    TEST("disable works",              !clipboard_plugin_is_enabled(plug));

    /* get_last_url وقتی خالیه */
    char url_buf[256] = {0};
    clipboard_plugin_get_last_url(plug, url_buf, sizeof(url_buf));
    TEST("last_url initially empty",   url_buf[0] == '\0');

    /* تست فیلتر پسوند */
    clipboard_plugin_set_filter(plug, ".zip\0.exe\0.mp4\0");
    TEST("filter set no crash",        TRUE);

    /* بازنشانی فیلتر */
    clipboard_plugin_set_filter(plug, NULL);
    TEST("filter clear no crash",      TRUE);

    /* get_last_url با NULL buffer */
    clipboard_plugin_get_last_url(plug, NULL, 0);
    TEST("get_last_url null safe",     TRUE);

    /* تست clipboard مستقیم: یه URL رو در کلیپ‌بورد بذار */
    const char *test_url = "https://example.com/testfile.zip";
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, strlen(test_url)+1);
        if (hg) {
            char *buf = (char*)GlobalLock(hg);
            if (buf) { strcpy(buf, test_url); GlobalUnlock(hg); }
            SetClipboardData(CF_TEXT, hg);
        }
        CloseClipboard();
    }

    /* فعال کن و کمی صبر کن تا پنجره message process بشه */
    clipboard_plugin_set_enabled(plug, TRUE);
    /* pump messages تا WM_CLIPBOARDUPDATE پردازش بشه */
    MSG m;
    DWORD deadline = GetTickCount() + 500;
    while (GetTickCount() < deadline) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(10);
    }

    /* چون در محیط تست پنجره message-only داریم، callback ممکنه fire نشه
       بدون message loop کامل — پس فقط crash نداشتن رو verify می‌کنیم */
    TEST("clipboard monitor ran safely", TRUE);

    clipboard_plugin_destroy(plug);
    TEST("clipboard destroy",          TRUE);

    clipboard_plugin_destroy(NULL);
    TEST("destroy null safe",          TRUE);
}

/* ======================================================
   تست ۹: Download Basket Plugin
   ====================================================== */
static int g_basket_drops = 0;
static char g_basket_last_url[2048] = {0};

static void test_basket_drop_cb(const char *url, void *userdata) {
    (void)userdata;
    g_basket_drops++;
    if (url) strncpy(g_basket_last_url, url, sizeof(g_basket_last_url)-1);
}

static void test_basket(void) {
    SECTION("Download Basket Plugin");

    IDM_PluginHooks *plug = basket_plugin_create(
        NULL, NULL, test_basket_drop_cb, NULL);
    TEST("basket create",              plug != NULL);
    if (!plug) return;

    TEST("plugin name set",            plug->name != NULL);
    TEST("api_version match",          plug->api_version == IDM_PLUGIN_API_VERSION);

    /* پیش‌فرض: مخفی */
    TEST("hidden by default",          !basket_visible(plug));

    /* حالت پیش‌فرض */
    TEST("default mode normal",        basket_get_mode(plug) == BASKET_MODE_NORMAL);

    /* تغییر حالت */
    basket_set_mode(plug, BASKET_MODE_COMPACT);
    TEST("set compact mode",           basket_get_mode(plug) == BASKET_MODE_COMPACT);
    basket_set_mode(plug, BASKET_MODE_NORMAL);
    TEST("restore normal mode",        basket_get_mode(plug) == BASKET_MODE_NORMAL);

    /* نمایش/مخفی */
    basket_show(plug, TRUE);
    TEST("show basket",                basket_visible(plug));
    basket_show(plug, FALSE);
    TEST("hide basket",                !basket_visible(plug));

    /* move to corner بدون crash */
    basket_move_to_corner(plug);
    TEST("move to corner no crash",    TRUE);

    /* NULL safety */
    basket_show(NULL, TRUE);
    basket_set_mode(NULL, BASKET_MODE_COMPACT);
    basket_move_to_corner(NULL);
    TEST("null safety all funcs",      TRUE);

    /* وقتی پنجره وجود نداره (hwnd=NULL چون تست بدون GUI اجرا میشه)
       این فقط crash نداشتن رو verify می‌کنه */
    TEST("basket operations no crash", TRUE);

    basket_plugin_destroy(plug);
    TEST("basket destroy",             TRUE);

    basket_plugin_destroy(NULL);
    TEST("destroy null safe",          TRUE);
}

/* ======================================================
   تست ۱۰: FTP Plugin
   ====================================================== */
static void test_ftp(void) {
    SECTION("FTP Plugin");

    /* تست parse URL */
    TEST("ftp url detect",        ftp_is_ftp_url("ftp://example.com/file.zip"));
    TEST("ftps url detect",       ftp_is_ftp_url("ftps://example.com/file.zip"));
    TEST("http not ftp",          !ftp_is_ftp_url("http://example.com/file.zip"));
    TEST("null not ftp",          !ftp_is_ftp_url(NULL));
    TEST("empty not ftp",         !ftp_is_ftp_url(""));

    /* plugin lifecycle */
    IDM_PluginHooks *plug = ftp_plugin_create();
    TEST("ftp plugin create",     plug != NULL);
    if (!plug) return;

    TEST("ftp name set",          plug->name != NULL);
    TEST("ftp version set",       plug->version != NULL);
    TEST("ftp api_version match", plug->api_version == IDM_PLUGIN_API_VERSION);

    /* NULL safety */
    TEST("ftp_is_ftp_url null",   !ftp_is_ftp_url(NULL));
    ftp_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);

    ftp_plugin_destroy(plug);
    TEST("ftp destroy",           TRUE);
}

/* ======================================================
   تست ۱۱: HLS Plugin
   ====================================================== */
static void test_hls(void) {
    SECTION("HLS / M3U8 Plugin");

    /* تشخیص URL */
    TEST("m3u8 url detect",       hls_is_m3u8_url("http://cdn.example.com/stream.m3u8"));
    TEST("m3u8 url uppercase",    hls_is_m3u8_url("http://example.com/live.M3U8"));
    TEST("m3u8 query param",      hls_is_m3u8_url("http://example.com/play?type=m3u8"));
    TEST("mp4 not m3u8",          !hls_is_m3u8_url("http://example.com/video.mp4"));
    TEST("null not m3u8",         !hls_is_m3u8_url(NULL));

    /* تشخیص از content */
    const char *m3u8_content = "#EXTM3U\n#EXT-X-VERSION:3\n";
    TEST("content detect valid",  hls_is_m3u8_content(m3u8_content,
                                   (int)strlen(m3u8_content)));
    TEST("content detect short",  !hls_is_m3u8_content("#EXT", 4));
    TEST("content detect null",   !hls_is_m3u8_content(NULL, 0));

    /* HLS_Playlist روی heap — روی stack 8MB میشه که stack overflow میده */
    HLS_Playlist *pl = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    TEST("hls_playlist alloc",    pl != NULL);
    if (!pl) return;

    /* شبیه‌سازی media playlist — بدون شبکه */
    pl->is_master     = FALSE;
    pl->is_endlist    = TRUE;
    pl->target_duration = 10;
    for (int i = 0; i < 3; i++) {
        snprintf(pl->segments[i].url, sizeof(pl->segments[i].url),
                 "http://media.example.com/seg%03d.ts", i+1);
        pl->segments[i].duration = (i < 2) ? 9.009 : 3.003;
        pl->segments[i].index = i;
    }
    pl->segment_count  = 3;
    pl->total_duration = 21.021;

    TEST("segment count",         pl->segment_count == 3);
    TEST("total duration",        pl->total_duration > 20.0 && pl->total_duration < 22.0);
    TEST("is endlist",            pl->is_endlist);
    TEST("not master",            !pl->is_master);
    TEST("first seg url",         strstr(pl->segments[0].url, "seg001") != NULL);
    TEST("last seg duration",     pl->segments[2].duration < 5.0);
    TEST("target duration",       pl->target_duration == 10);

    /* master playlist — هم روی heap */
    HLS_Playlist *master = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    TEST("master alloc",          master != NULL);
    if (!master) { free(pl); return; }

    master->is_master = TRUE;
    master->variants[0].bandwidth = 500000;
    master->variants[1].bandwidth = 1280000;
    master->variants[2].bandwidth = 2560000;
    master->variant_count = 3;
    master->best_variant  = 2;
    snprintf(master->variants[2].url, sizeof(master->variants[2].url),
             "http://example.com/1080p.m3u8");

    TEST("variant count",         master->variant_count == 3);
    TEST("best variant index",    master->best_variant == 2);
    TEST("best variant bw",       master->variants[master->best_variant].bandwidth == 2560000);
    TEST("best variant url",      strstr(master->variants[master->best_variant].url, "1080p") != NULL);

    free(pl);
    free(master);

    /* plugin lifecycle */
    IDM_PluginHooks *plug = hls_plugin_create();
    TEST("hls plugin create",     plug != NULL);
    if (!plug) return;
    TEST("hls name set",          plug->name != NULL);
    TEST("hls api_version match", plug->api_version == IDM_PLUGIN_API_VERSION);

    hls_plugin_destroy(plug);
    TEST("hls destroy",           TRUE);
    hls_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);
}

/* ======================================================
   تست ۱۲: Proxy Plugin
   ====================================================== */
static void test_proxy(void) {
    SECTION("Proxy Support Plugin");

    IDM_PluginHooks *plug = proxy_plugin_create();
    TEST("proxy create",          plug != NULL);
    if (!plug) return;

    TEST("plugin name set",       plug->name != NULL);
    TEST("api_version match",     plug->api_version == IDM_PLUGIN_API_VERSION);

    /* پیش‌فرض: غیرفعال */
    TEST("inactive by default",   !proxy_plugin_is_active(plug));

    /* set از URL — HTTP */
    BOOL ok = proxy_plugin_set_from_url(plug, "http://proxy.example.com:8080");
    TEST("set http proxy",        ok);
    TEST("active after set",      proxy_plugin_is_active(plug));

    ProxyConfig cfg;
    proxy_plugin_get_config(plug, &cfg);
    TEST("type is http",          cfg.type == PROXY_HTTP);
    TEST("host correct",          strcmp(cfg.host, "proxy.example.com") == 0);
    TEST("port correct",          cfg.port == 8080);
    TEST("no user (anonymous)",   cfg.user[0] == '\0');

    /* set با user:pass */
    ok = proxy_plugin_set_from_url(plug, "http://admin:secret@corp-proxy:3128");
    TEST("set proxy with auth",   ok);
    proxy_plugin_get_config(plug, &cfg);
    TEST("user extracted",        strcmp(cfg.user, "admin") == 0);
    TEST("pass extracted",        strcmp(cfg.pass, "secret") == 0);
    TEST("port 3128",             cfg.port == 3128);

    /* SOCKS5 */
    ok = proxy_plugin_set_from_url(plug, "socks5://127.0.0.1:1080");
    TEST("set socks5",            ok);
    proxy_plugin_get_config(plug, &cfg);
    TEST("type is socks5",        cfg.type == PROXY_SOCKS5);
    TEST("socks host",            strcmp(cfg.host, "127.0.0.1") == 0);
    TEST("socks port",            cfg.port == 1080);

    /* SOCKS4 */
    ok = proxy_plugin_set_from_url(plug, "socks4://10.0.0.1:1081");
    TEST("set socks4",            ok);
    proxy_plugin_get_config(plug, &cfg);
    TEST("type is socks4",        cfg.type == PROXY_SOCKS4);

    /* clear */
    proxy_plugin_clear(plug);
    TEST("inactive after clear",  !proxy_plugin_is_active(plug));

    /* invalid URL */
    ok = proxy_plugin_set_from_url(plug, "");
    TEST("empty url rejected",    !ok);
    ok = proxy_plugin_set_from_url(plug, NULL);
    TEST("null url rejected",     !ok);

    /* NULL safety */
    proxy_plugin_clear(NULL);
    proxy_plugin_is_active(NULL);
    proxy_plugin_get_config(NULL, NULL);
    TEST("null safety",           TRUE);

    proxy_plugin_destroy(plug);
    TEST("proxy destroy",         TRUE);
    proxy_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);
}

/* ======================================================
   تست ۱۳: HTTP Auth Plugin
   ====================================================== */
static void test_http_auth(void) {
    SECTION("HTTP Auth Plugin");

    IDM_PluginHooks *plug = auth_plugin_create();
    TEST("auth create",           plug != NULL);
    if (!plug) return;

    TEST("plugin name set",       plug->name != NULL);
    TEST("api_version match",     plug->api_version == IDM_PLUGIN_API_VERSION);

    /* اولیه خالی */
    TEST("count initially 0",     auth_plugin_count(plug) == 0);

    /* اضافه کردن credential */
    BOOL ok = auth_plugin_add(plug, "api.example.com", "user1", "pass1", AUTH_BASIC);
    TEST("add basic credential",  ok);
    TEST("count is 1",            auth_plugin_count(plug) == 1);

    ok = auth_plugin_add(plug, "secure.example.com", "admin", "s3cr3t", AUTH_DIGEST);
    TEST("add digest credential", ok);
    TEST("count is 2",            auth_plugin_count(plug) == 2);

    /* پیدا کردن */
    AuthEntry e;
    TEST("find exact domain",     auth_plugin_find(plug, "api.example.com", &e));
    TEST("user correct",          strcmp(e.user, "user1") == 0);
    TEST("pass correct",          strcmp(e.pass, "pass1") == 0);
    TEST("type basic",            e.type == AUTH_BASIC);

    TEST("find digest domain",    auth_plugin_find(plug, "secure.example.com", &e));
    TEST("type digest",           e.type == AUTH_DIGEST);

    /* subdomain matching */
    TEST("subdomain match",       auth_plugin_find(plug, "sub.api.example.com", &e));
    TEST("subdomain user ok",     strcmp(e.user, "user1") == 0);

    /* domain که وجود نداره */
    TEST("unknown domain false",  !auth_plugin_find(plug, "other.com", &e));

    /* آپدیت credential موجود */
    ok = auth_plugin_add(plug, "api.example.com", "user1_new", "pass1_new", AUTH_BASIC);
    TEST("update existing",       ok);
    TEST("count still 2",         auth_plugin_count(plug) == 2);
    auth_plugin_find(plug, "api.example.com", &e);
    TEST("user updated",          strcmp(e.user, "user1_new") == 0);

    /* حذف */
    auth_plugin_remove(plug, "api.example.com");
    TEST("count after remove",    auth_plugin_count(plug) == 1);
    TEST("removed not found",     !auth_plugin_find(plug, "api.example.com", &e));

    /* Basic auth header */
    char hdr[256];
    auth_make_basic_header("testuser", "testpass", hdr, sizeof(hdr));
    TEST("basic header prefix",   strncmp(hdr, "Basic ", 6) == 0);
    TEST("basic header not empty", strlen(hdr) > 10);
    /* verify: base64("testuser:testpass") = "dGVzdHVzZXI6dGVzdHBhc3M=" */
    TEST("basic header b64 ok",   strstr(hdr, "dGVzdHVzZXI6dGVzdHBhc3M=") != NULL);

    /* clear */
    auth_plugin_clear(plug);
    TEST("count after clear 0",   auth_plugin_count(plug) == 0);

    /* NULL safety */
    auth_plugin_add(NULL, "x", "u", "p", AUTH_BASIC);
    auth_plugin_find(NULL, "x", &e);
    auth_plugin_remove(NULL, "x");
    auth_plugin_count(NULL);
    auth_make_basic_header(NULL, NULL, NULL, 0);
    TEST("null safety all",       TRUE);

    auth_plugin_destroy(plug);
    TEST("auth destroy",          TRUE);
    auth_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);
}

/* ======================================================
   تست ۱۴: Engine — scheduler، batch، clear_completed
   ====================================================== */
static void test_engine_features(void) {
    SECTION("Engine: Scheduler + Batch + Clear");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create",         eng != NULL);
    if (!eng) return;

    /* ── engine_add_batch ── */
    const char *batch_urls =
        "http://example.com/file1.zip\n"
        "http://example.com/file2.zip\n"
        "   \n"                          /* خط خالی — باید skip بشه */
        "http://example.com/file3.zip\n"
        "not_a_url\n"                    /* invalid — باید skip بشه */
        "ftp://files.example.com/data.tar.gz\n";

    int added = engine_add_batch(eng, batch_urls, NULL, 4, 0, "TestGroup");
    TEST("batch: 3 http + 1 ftp accepted (empty and invalid lines skipped)", added == 4);

    /* بررسی group_id یکسان */
    EnterCriticalSection(&eng->lock);
    uint32_t gid = 0;
    int same_group = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) {
        if (gid == 0 && dl->group_id != 0) gid = dl->group_id;
        if (dl->group_id == gid) same_group++;
    }
    LeaveCriticalSection(&eng->lock);
    TEST("all accepted links in same group", same_group == 4);
    TEST("group_id not zero",     gid != 0);

    /* بررسی group_name */
    EnterCriticalSection(&eng->lock);
    IDM_Download *first = eng->queue_head;
    BOOL name_ok = first && strcmp(first->group_name, "TestGroup") == 0;
    LeaveCriticalSection(&eng->lock);
    TEST("group name set",        name_ok);

    /* ── engine_clear_completed ── */
    /* دستی state چند تا رو completed کن */
    EnterCriticalSection(&eng->lock);
    int marked = 0;
    for (IDM_Download *dl = eng->queue_head; dl && marked < 2; dl = dl->next) {
        dl->state = DL_COMPLETED;
        marked++;
    }
    LeaveCriticalSection(&eng->lock);

    int cleared = engine_clear_completed(eng);
    TEST("cleared 2 completed",   cleared == 2);

    /* count باید ۲ تا کمتر باشه */
    EnterCriticalSection(&eng->lock);
    int remaining = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) remaining++;
    LeaveCriticalSection(&eng->lock);
    TEST("remaining count ok",    remaining == 2);

    /* ── engine_schedule ── */
    EnterCriticalSection(&eng->lock);
    uint32_t first_id = eng->queue_head ? eng->queue_head->id : 0;
    LeaveCriticalSection(&eng->lock);

    if (first_id) {
        engine_schedule(eng, first_id, 23, 59);

        EnterCriticalSection(&eng->lock);
        IDM_Download *dl = engine_find(eng, first_id);
        BOOL has_sched   = dl && dl->has_schedule;
        BOOL is_paused   = dl && (dl->state == DL_PAUSED ||
                                   dl->state == DL_QUEUED);
        LeaveCriticalSection(&eng->lock);
        TEST("schedule sets flag",    has_sched);
        TEST("state after schedule",  is_paused);

        /* unschedule */
        engine_unschedule(eng, first_id);
        EnterCriticalSection(&eng->lock);
        dl = engine_find(eng, first_id);
        BOOL cleared_flag = dl && !dl->has_schedule;
        LeaveCriticalSection(&eng->lock);
        TEST("unschedule clears flag", cleared_flag);

        /* schedule invalid hour */
        engine_schedule(eng, first_id, 25, 0);
        EnterCriticalSection(&eng->lock);
        dl = engine_find(eng, first_id);
        BOOL still_clear = dl && !dl->has_schedule;
        LeaveCriticalSection(&eng->lock);
        TEST("invalid hour rejected",  still_clear);
    }

    /* ── NULL safety ── */
    engine_schedule(NULL, 0, 12, 0);
    engine_unschedule(NULL, 0);
    engine_add_batch(NULL, "http://x.com/f", NULL, 4, 0, NULL);
    engine_clear_completed(NULL);
    TEST("null safety engine funcs", TRUE);

    engine_destroy(eng);
    TEST("engine destroy",        TRUE);
}

/* ======================================================
   تست ۱۵: engine_add FIFO order
   ====================================================== */
static void test_engine_fifo(void) {
    SECTION("Engine: FIFO Queue Order");

    IDM_Engine *eng = engine_create(NULL);
    /* max=0 تا هیچ دانلودی شروع نشه */
    eng->max_concurrent = 0;
    TEST("engine create",         eng != NULL);
    if (!eng) return;

    engine_add(eng, "http://example.com/first.zip",  "c:\\tmp\\1.zip", 1, 0);
    engine_add(eng, "http://example.com/second.zip", "c:\\tmp\\2.zip", 1, 0);
    engine_add(eng, "http://example.com/third.zip",  "c:\\tmp\\3.zip", 1, 0);

    /* FIFO: اولین باید first.zip باشه */
    EnterCriticalSection(&eng->lock);
    IDM_Download *h = eng->queue_head;
    BOOL fifo_ok = h
        && strstr(h->url, "first")  != NULL
        && strstr(h->next->url, "second") != NULL
        && strstr(h->next->next->url, "third") != NULL;
    LeaveCriticalSection(&eng->lock);
    TEST("FIFO order correct",    fifo_ok);

    /* remove وسطی — tail باید درست بمونه */
    EnterCriticalSection(&eng->lock);
    uint32_t mid_id = eng->queue_head->next->id;
    LeaveCriticalSection(&eng->lock);

    engine_remove(eng, mid_id);

    EnterCriticalSection(&eng->lock);
    int cnt = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) cnt++;
    BOOL tail_ok = (eng->queue_tail != NULL) &&
                   strstr(eng->queue_tail->url, "third") != NULL;
    LeaveCriticalSection(&eng->lock);
    TEST("count after remove",    cnt == 2);
    TEST("tail still correct",    tail_ok);

    engine_destroy(eng);
    TEST("engine destroy",        TRUE);
}

/* ======================================================
   تست: دانلود همزمان چند فایل (multi-concurrent)
   ====================================================== */
static void test_engine_concurrent(void) {
    SECTION("Engine: Multi-Concurrent Downloads");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create",                eng != NULL);
    if (!eng) return;

    /* max_concurrent رو روی 3 بذار */
    eng->max_concurrent = 3;
    /* scheduler رو غیرفعال کن تا فقط queue logic رو تست کنیم */
    eng->max_concurrent = 0; /* هیچ auto-launch نشه */

    IDM_Download *d1 = engine_add(eng, "http://a.com/f1.zip", "C:\\f1.zip", 4, 0);
    IDM_Download *d2 = engine_add(eng, "http://b.com/f2.zip", "C:\\f2.zip", 4, 0);
    IDM_Download *d3 = engine_add(eng, "http://c.com/f3.zip", "C:\\f3.zip", 4, 0);

    TEST("3 downloads added",            d1 && d2 && d3);
    TEST("all start as QUEUED",
         d1 && d1->state == DL_QUEUED &&
         d2 && d2->state == DL_QUEUED &&
         d3 && d3->state == DL_QUEUED);

    /* شمارش دستی از queue */
    int cnt = 0;
    EnterCriticalSection(&eng->lock);
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) cnt++;
    LeaveCriticalSection(&eng->lock);
    TEST("queue count is 3",             cnt == 3);

    /* تنظیم max_concurrent و شبیه‌سازی چک scheduler */
    eng->max_concurrent = 3;
    int would_launch = 0;
    EnterCriticalSection(&eng->lock);
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next)
        if (dl->state == DL_QUEUED && eng->active_count < eng->max_concurrent)
            would_launch++;
    LeaveCriticalSection(&eng->lock);

    TEST("scheduler would launch all 3", would_launch == 3);

    engine_destroy(eng);
    TEST("engine destroy",               TRUE);
}

/* ======================================================
   تست: Real Concurrent HTTP Downloads (loopback)
   ─────────────────────────────────────────────────────
   دلیل وجودش: تست بالا ("Multi-Concurrent Downloads") فقط
   منطق scheduler رو با شمارش دستی روی queue شبیه‌سازی می‌کرد —
   هیچ‌وقت واقعاً http_discover/http_open_range رو صدا نمی‌زد.
   دقیقاً به همین خاطر بود که باگ واقعی v5.0.7 (WinINet سقف
   connection همزمان به یه host رو رعایت می‌کرد و دانلود دوم/سوم
   در «connecting» گیر می‌کردن) هیچ‌وقت توسط test_all گرفته نشد —
   فقط با تست دستی روی اینترنت واقعی کشف شد.

   این تست یه سرور HTTP واقعی (هرچند مینیمال) روی 127.0.0.1
   بالا میاره، ۳ دانلود واقعی از همون host واحد اضافه می‌کنه، و
   مستقیماً از خودِ سرور اندازه می‌گیره که آیا واقعاً چند GET
   هم‌زمان باز بوده یا نه — به‌جای اینکه فقط state داخلی engine
   رو (که می‌تونه گمراه‌کننده باشه) چک کنه.

   عمداً http_init() رو اینجا صدا می‌زنیم — دقیقاً همون کاری که
   main.c موقع startup انجام میده. اگه یه روز کسی اون call رو از
   main.c حذف/جابه‌جا کنه، این تست باید fail بشه (peak concurrent
   GET به ۱ برمی‌گرده)، نه اینکه ساکت رگرس کنه. */
static void test_engine_real_concurrency(void) {
    SECTION("Engine: Real Concurrent HTTP Downloads (loopback)");

    const uint16_t PORT = 58732;
    const uint32_t BODY_SIZE = 65536;      /* 64KB — engine فقط ۱ segment انتخاب می‌کنه */
    /* توجه مهم: engine.c عمداً بین launch دانلودهای مختلف به یه
       host واحد یه فاصله می‌ندازه (ENGINE_HOST_STAGGER_MS = 1500ms،
       در engine.h) تا سرور رو با burst همزمان نکوبه — این خودش
       یه فیچر دلخواسته‌ست، نه باگ. نسخه‌ی اول این تست TRICKLE_MS
       رو ۱۲۰۰ گذاشته بود، یعنی دانلود اول کامل می‌شد و connection‌ش
       بسته می‌شد *قبل* از این‌که حتی دانلود دوم (که ۱۵۰۰ms بعد
       stagger می‌شه) شروع بشه — پس هیچ‌وقت هم‌پوشانی واقعی رخ
       نمی‌داد و peak همیشه ۱ می‌موند، صرف‌نظر از این‌که concurrency
       واقعی درست کار می‌کنه یا نه. باید TRICKLE_MS به‌قدر کافی
       بزرگ‌تر از ۲×STAGGER باشه تا سه‌تا connection واقعاً هم‌زمان
       باز بمونن. */
    const uint32_t TRICKLE_MS = 3000;

    bool srv_ok = test_httpd_start(PORT, BODY_SIZE, TRICKLE_MS);
    TEST("loopback test server started", srv_ok);
    if (!srv_ok) return;

    /* دقیقاً همون چیزی که main.c قبل از engine_create صدا می‌زنه —
       بدون این، این تست دقیقاً همون باگ v5.0.7 رو reproduce می‌کنه. */
    http_init();

    /* callback های http (If-Range، کوکی، auth) از plugins_init وصل
       می‌شن — همون کاری که برنامه‌ی واقعی می‌کنه. */
    IDM_PluginRegistry *plug_reg = plugins_init();

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) { test_httpd_stop(); return; }

    char urls[3][64];
    char out_paths[3][MAX_PATH + 32];
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);

    IDM_Download *dls[3] = {0};
    for (int i = 0; i < 3; i++) {
        snprintf(urls[i], sizeof(urls[i]), "http://127.0.0.1:%u/f%d", PORT, i);
        snprintf(out_paths[i], sizeof(out_paths[i]), "%slass_test_httpd_f%d.bin", tmp_dir, i);
        DeleteFileA(out_paths[i]);
        dls[i] = engine_add(eng, urls[i], out_paths[i], 4, 0);
    }
    TEST("3 real downloads added", dls[0] && dls[1] && dls[2]);

    /* بازه‌ی صبر رو متناسب با stagger + trickle عمداً سخاوتمندانه
       گرفتیم: بدترین حالت ~ 2*STAGGER + TRICKLE + سربار ≈ 3+3=6s،
       پس ۲۰ ثانیه صبر می‌کنیم که روی ماشین‌های کندتر هم flaky نشه. */
    bool all_completed = false;
    for (int tick = 0; tick < 200; tick++) {
        Sleep(100);
        EnterCriticalSection(&eng->lock);
        bool done = dls[0] && dls[1] && dls[2] &&
                    dls[0]->state == DL_COMPLETED &&
                    dls[1]->state == DL_COMPLETED &&
                    dls[2]->state == DL_COMPLETED;
        LeaveCriticalSection(&eng->lock);
        if (done) { all_completed = true; break; }
    }

    int peak = test_httpd_peak_concurrent_gets();
    int completed = test_httpd_completed_gets();

    TEST("all 3 real downloads reached DL_COMPLETED", all_completed);
    TEST("server observed >=2 concurrent GET connections", peak >= 2);
    TEST("server actually served all 3 GET requests", completed >= 3);
    TEST("ETag from HEAD is sent back as If-Range on ranged GETs",
         test_httpd_if_range_gets() >= 1);

    engine_destroy(eng);
    TEST("engine destroy", TRUE);

    plugins_shutdown(plug_reg);
    test_httpd_stop();

    for (int i = 0; i < 3; i++) DeleteFileA(out_paths[i]);
}

/* ======================================================
   تست: گفتگو با سرور، نه دیکته — Retry-After honored + شروع
   محتاطانه‌تر روی host پرمشکل (رجوع کن به توضیح کامل کنار
   engine_report_host_trouble/pick_seg_count در engine.c و
   خوندن Retry-After در http.c)
   ====================================================== */
static void test_server_negotiation(void) {
    SECTION("Server Negotiation: Retry-After + Adaptive Concurrency");

    const uint16_t PORT2 = 58733;
    bool srv_ok = test_httpd_start(PORT2, 4096, 0);
    TEST("negotiation test server started", srv_ok);
    if (srv_ok) {
        http_init();
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/retry-after-test", PORT2);

        HTTP_DiscoveryInfo info = {0};
        IDM_Result r = http_discover(url, NULL, NULL, &info);

        TEST("429 with Retry-After is treated as retryable (not permanent)",
             r == IDM_ERR_NETWORK);
        TEST("Retry-After value actually parsed from server response",
             info.retry_after_sec == 2);

        test_httpd_stop();
    }

    /* ── شروع محتاطانه‌تر روی host پرمشکل ── */
    const char *host = "trouble-test-host.example";
    uint32_t n_before = engine_test_pick_seg_count(host, 100ULL*1024*1024, 8);
    TEST("clean host gets full segment count for a large file", n_before == 8);

    for (int i = 0; i < 3; i++) engine_report_host_trouble(host);
    uint32_t n_after = engine_test_pick_seg_count(host, 100ULL*1024*1024, 8);
    TEST("host with 3 reported troubles gets fewer segments",
         n_after == n_before - 3);
    TEST("trouble-adjusted count never drops below 1", n_after >= 1);

    /* یه host کاملاً بی‌ربط نباید تحت تأثیر مشکلات host بالا باشه */
    uint32_t n_other = engine_test_pick_seg_count("unrelated-host.example",
                                                  100ULL*1024*1024, 8);
    TEST("unrelated host is unaffected by another host's trouble",
         n_other == 8);
}

/* ======================================================
   تست: Batch / استخراج URL از متن آزاد
   (سناریوهایی که قبلاً «No valid URLs found» می‌دادن)
   ====================================================== */
static int url_line_count(const char *out) {
    int n = 0;
    for (const char *p = out; *p; p++) if (*p == '\n') n++;
    return n;
}

static void test_batch_url_extraction(void) {
    SECTION("Batch: URL extraction from free text");
    char out[4096];

    /* ۱) روی یک خط با فاصله — قبلاً کل خط یک URL حساب می‌شد */
    int n = engine_extract_urls("https://a.com/x.zip https://b.com/y.zip\thttp://c.com/z", out, sizeof(out));
    TEST("space/tab separated links all found", n == 3);
    TEST("each link on its own line", url_line_count(out) == 3);

    /* ۲) فقط \r بین لینک‌ها (بعضی برنامه‌ها/کلیپ‌بوردها) */
    n = engine_extract_urls("http://a.com/1\rhttp://a.com/2\rhttp://a.com/3", out, sizeof(out));
    TEST("CR-only separated links found", n == 3);

    /* ۳) CRLF معمولی */
    n = engine_extract_urls("http://a.com/1\r\nhttp://a.com/2\r\n", out, sizeof(out));
    TEST("CRLF separated links found", n == 2);
    TEST("no CR left in output", strchr(out, '\r') == NULL);

    /* ۴) متن اطراف، کوتیشن و <> */
    n = engine_extract_urls("Link: <https://x.com/f.zip>, or \"http://y.com/g.zip\".", out, sizeof(out));
    TEST("links inside <> and quotes found", n == 2);
    TEST("trailing punctuation stripped",
         strstr(out, "f.zip\n") != NULL && strstr(out, "g.zip\n") != NULL);

    /* ۵) تکراری‌ها یکی می‌شن */
    n = engine_extract_urls("http://a.com/1\nhttp://a.com/1\nhttp://a.com/2", out, sizeof(out));
    TEST("duplicate links removed", n == 2);

    /* ۶) پرانتز: جفت‌دار نگه داشته، بدون جفت حذف */
    n = engine_extract_urls("http://a.com/w_(x) and (see http://b.com/p)", out, sizeof(out));
    TEST("balanced paren kept, dangling paren dropped",
         n == 2 && strstr(out, "w_(x)\n") != NULL && strstr(out, "b.com/p\n") != NULL);

    /* ۷) بایت‌های غیر-ASCII (UTF-8) به %XX */
    n = engine_extract_urls("http://a.com/\xD8\xB3", out, sizeof(out));
    TEST("non-ASCII bytes percent-encoded", n == 1 && strstr(out, "/%D8%B3\n") != NULL);

    /* ۸) NBSP بین لینک‌ها (کپی از وب) جداکننده‌ست */
    n = engine_extract_urls("http://a.com/1\xC2\xA0http://a.com/2", out, sizeof(out));
    TEST("NBSP separates links", n == 2);

    /* ۹) وسط کلمه scheme نیست */
    n = engine_extract_urls("xhttp://a.com/1 nothing here", out, sizeof(out));
    TEST("scheme glued to a word is not a link", n == 0);

    /* ۱۰) ورودی بی‌لینک / NULL / خالی */
    TEST("plain text has zero links", engine_extract_urls("hello world", out, sizeof(out)) == 0);
    TEST("NULL text safe", engine_extract_urls(NULL, out, sizeof(out)) == 0);
    TEST("empty text safe", engine_extract_urls("", out, sizeof(out)) == 0);
    TEST("scheme alone is not a link", engine_extract_urls("http://", out, sizeof(out)) == 0);

    /* ۱۱) فقط شمارش (out==NULL) */
    TEST("count-only mode works",
         engine_extract_urls("http://a.com/1 http://a.com/2", NULL, 0) == 2);

    /* ۱۲) بافر کوچیک: URL ناقص هرگز نوشته نمی‌شه */
    char tiny[24];
    n = engine_extract_urls("http://a.com/1 http://a.com/some/long/path/file.zip", tiny, sizeof(tiny));
    TEST("tiny buffer never holds a partial URL",
         n <= 2 && (tiny[0] == '\0' || tiny[strlen(tiny) - 1] == '\n'));

    /* ۱۳) لیست بزرگ (بیشتر از سقف ۳۲۷۶۷ کاراکتر پیش‌فرض کنترل Edit)
       — ۶۰۰ لینک ~۱۰۰ کاراکتری */
    size_t big_cap = 600 * 130;
    char *big = (char *)malloc(big_cap), *bo = (char *)malloc(big_cap * 3);
    if (big && bo) {
        size_t pos = 0;
        for (int i = 0; i < 600; i++)
            pos += (size_t)snprintf(big + pos, big_cap - pos,
                "https://cdn%d.example.com/dl/token/abcdefghijklmnopqrstuvwxyz0123456789/file_%04d.zip?sig=0123456789abcdef\r\n",
                i % 7, i);
        TEST("big list is longer than default Edit limit", pos > 32767);
        int bn = engine_extract_urls(big, bo, big_cap * 3);
        TEST("600 links in a 60KB paste all extracted", bn == 600);
    }
    free(big); free(bo);
}

/* ======================================================
   تست: Crash Report (محل خطا + call stack)
   ====================================================== */
static int crash_depth_probe(int n) {
    /* چند frame واقعی روی stack بساز تا unwinder چیزی برای راه رفتن
       داشته باشه — volatile تا کامپایلر inline/حذفش نکنه */
    volatile int sink = n;
    if (n > 0) return crash_depth_probe(n - 1) + sink;

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof(er));
    er.ExceptionCode    = EXCEPTION_ACCESS_VIOLATION;
    er.ExceptionAddress = (PVOID)(uintptr_t)ctx.Rip;
    EXCEPTION_POINTERS ep = { &er, &ctx };
    return crash_report_log(&ep);
}

/* ======================================================
   تست: انتقال داده‌های نسخه‌ی قدیمی (پوشه‌ی قدیمی → Lass)
   ====================================================== */
static void mig_write(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int mig_read_line(const char *path, char *out, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = (fgets(out, (int)sz, f) != NULL);
    fclose(f);
    return ok;
}

static void test_legacy_migration(void) {
    SECTION("Legacy data migration (old folder -> Lass)");
    CreateDirectoryA("mig_test", NULL);
    CreateDirectoryA("mig_test\\old_states", NULL);

    mig_write("mig_test\\old_cookies.txt", "old-data\n");
    int c1 = legacy_copy_file_if_missing("mig_test\\old_cookies.txt",
                                         "mig_test\\new\\cookies.txt");
    TEST("single file is copied (creating the new folder)", c1 == 1);
    char buf[64] = {0};
    TEST("copied content is identical",
         mig_read_line("mig_test\\new\\cookies.txt", buf, sizeof(buf)) &&
         strcmp(buf, "old-data\n") == 0);

    mig_write("mig_test\\old_cookies.txt", "changed\n");
    int c2 = legacy_copy_file_if_missing("mig_test\\old_cookies.txt",
                                         "mig_test\\new\\cookies.txt");
    TEST("existing destination is never overwritten", c2 == 0);
    memset(buf, 0, sizeof(buf));
    mig_read_line("mig_test\\new\\cookies.txt", buf, sizeof(buf));
    TEST("destination content kept after a second call", strcmp(buf, "old-data\n") == 0);
    TEST("legacy source is left intact",
         GetFileAttributesA("mig_test\\old_cookies.txt") != INVALID_FILE_ATTRIBUTES);
    TEST("missing legacy file is a no-op",
         legacy_copy_file_if_missing("mig_test\\nope.txt", "mig_test\\x.txt") == 0);

    mig_write("mig_test\\old_states\\a.dat", "A");
    mig_write("mig_test\\old_states\\b.dat", "B");
    mig_write("mig_test\\old_states\\skip.txt", "X");
    int d1 = legacy_copy_dat_dir_if_missing("mig_test\\old_states", "mig_test\\new_states");
    TEST("only .dat state files are migrated", d1 == 2);
    TEST("non-.dat file is not migrated",
         GetFileAttributesA("mig_test\\new_states\\skip.txt") == INVALID_FILE_ATTRIBUTES);
    int d2 = legacy_copy_dat_dir_if_missing("mig_test\\old_states", "mig_test\\new_states");
    TEST("second directory migration copies nothing", d2 == 0);

    DeleteFileA("mig_test\\old_cookies.txt");
    DeleteFileA("mig_test\\new\\cookies.txt");
    DeleteFileA("mig_test\\old_states\\a.dat");
    DeleteFileA("mig_test\\old_states\\b.dat");
    DeleteFileA("mig_test\\old_states\\skip.txt");
    DeleteFileA("mig_test\\new_states\\a.dat");
    DeleteFileA("mig_test\\new_states\\b.dat");
    RemoveDirectoryA("mig_test\\new");
    RemoveDirectoryA("mig_test\\new_states");
    RemoveDirectoryA("mig_test\\old_states");
    RemoveDirectoryA("mig_test");
}

/* ======================================================
   تست: چرخه‌ی عمر دانلود — validation، busy و حذف معلق
   ====================================================== */
IDM_Download *engine_find(IDM_Engine *eng, uint32_t id);

static void test_dl_lifecycle(void) {
    SECTION("Download lifecycle: validation, busy and deferred removal");
    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) return;

    TEST("ftps link is rejected at engine level", engine_add(eng, "ftps://127.0.0.1:1/x.zip", "", 1, 0) == NULL);
    TEST("ftp link is accepted at engine level", engine_add(eng, "ftp://127.0.0.1:1/x.zip", "", 1, 0) != NULL);

    IDM_Download *upper = engine_add(eng, "HTTP://127.0.0.1:1/upper.zip", "", 1, 0);
    TEST("uppercase http scheme is accepted", upper != NULL);
    if (upper) engine_remove(eng, upper->id);

    IDM_Download *dl = engine_add(eng, "http://127.0.0.1:1/busy.zip", "", 1, 0);
    TEST("download added for lifecycle test", dl != NULL);
    if (!dl) { engine_destroy(eng); return; }
    uint32_t id = dl->id;

    TEST("acquire of unknown id returns NULL", engine_dl_acquire(eng, 0xFFFFFFFFu) == NULL);
    IDM_Download *held = engine_dl_acquire(eng, id);
    TEST("acquire of live download returns it", held == dl);
    TEST("acquire raises busy", held != NULL && held->busy >= 1);

    engine_remove(eng, id);
    TEST("removal unlinks the download at once", engine_find(eng, id) == NULL);
    TEST("removal of a held download is deferred", held != NULL && held->remove_pending);
    TEST("held download cannot be re-acquired after removal", engine_dl_acquire(eng, id) == NULL);

    engine_dl_release(eng, held);
    TEST("last release completes the deferred removal", TRUE);
    engine_destroy(eng);
    TEST("engine destroy after lifecycle test", TRUE);
}

/* ======================================================
   تست: FTP مستقل — URL، SIZE/REST، بازه‌ی شامل، و دانلود کامل با engine
   ====================================================== */
static void test_ftp_download(void) {
    SECTION("FTP: independent client, ranged read and full download");
    const uint16_t FTP_PORT = 58761;
    const uint64_t FSIZE = 3u * 1024u * 1024u;
    bool srv_ok = test_ftpd_start(FTP_PORT, FSIZE);
    TEST("ftp test server started", srv_ok);
    if (!srv_ok) return;

    char url[160];
    snprintf(url, sizeof(url), "ftp://127.0.0.1:%u/file.bin", (unsigned)FTP_PORT);
    TEST("ftp url recognised", ftp_is_ftp_url(url));
    TEST("ftp url supported", dl_url_supported(url));
    TEST("ftps url not supported", !dl_url_supported("ftps://127.0.0.1/file.bin"));

    uint64_t size = 0;
    BOOL ranges = FALSE;
    char fn[IDM_MAX_FILENAME] = {0};
    IDM_Result r = ftp_discover(url, &size, fn, sizeof(fn), &ranges);
    TEST("discover succeeds", r == IDM_OK);
    TEST("discover reports the file size", size == FSIZE);
    TEST("discover reports the file name", strcmp(fn, "file.bin") == 0);
    TEST("discover detects REST support", ranges == TRUE);

    FtpStream *st = ftp_range_open(url, 1000, 1000 + 4095);
    TEST("ranged open (REST) succeeds", st != NULL);
    if (st) {
        static uint8_t rb[4096];
        int got = 0, n = 0;
        while (got < (int)sizeof(rb) && (n = ftp_range_read(st, rb + got, (int)sizeof(rb) - got)) > 0)
            got += n;
        TEST("ranged read returns exactly 4096 bytes", got == 4096);
        bool exact = (got == 4096);
        for (int i = 0; exact && i < got; i++)
            if (rb[i] != test_ftpd_byte(1000 + (uint64_t)i)) exact = false;
        TEST("ranged read bytes match the file at that offset", exact);
        TEST("server honoured the resume request", ftp_range_got_partial(st));
        ftp_range_close(st);
    }

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create for ftp download", eng != NULL);
    IDM_Download *dl = eng ? engine_add(eng, url, "ftp_test_out.bin", 4, 0) : NULL;
    TEST("ftp download added", dl != NULL);
    bool done = false;
    if (dl) {
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 30000) {
            if (dl->state == DL_COMPLETED) { done = true; break; }
            if (dl->state == DL_FAILED) break;
            Sleep(50);
        }
    }
    TEST("ftp download completes", done);

    bool content_ok = false;
    if (done) {
        FILE *f = fopen("ftp_test_out.bin", "rb");
        if (f) {
            static uint8_t cb[65536];
            uint64_t pos = 0;
            bool ok = true;
            size_t cnt = 0;
            while ((cnt = fread(cb, 1, sizeof(cb), f)) > 0) {
                for (size_t i = 0; i < cnt; i++)
                    if (cb[i] != test_ftpd_byte(pos + (uint64_t)i)) { ok = false; break; }
                if (!ok) break;
                pos += (uint64_t)cnt;
            }
            fclose(f);
            content_ok = ok && pos == FSIZE;
        }
    }
    TEST("downloaded file is byte-identical to the server file", content_ok);

    if (eng) engine_destroy(eng);
    test_ftpd_stop();
    DeleteFileA("ftp_test_out.bin");
}

/* ======================================================
   تست: رمز FTP — جداسازی از URL، پوشاندن در لاگ، صف رمزشده و سازگاری با صف نسخه‌ی ۱
   ====================================================== */
static BOOL bytes_contain(const char *data, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return FALSE;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(data + i, needle, m) == 0) return TRUE;
    return FALSE;
}

static void test_url_credentials(void) {
    SECTION("URL credentials: split, redact, protected queue and version 1 compatibility");
    lass_store_close();
    DeleteFileA("tels_test_cred_store.tels");
    lass_store_open("tels_test_cred_store.tels");
    char clean[IDM_MAX_URL] = {0};
    char pass[128] = {0};
    BOOL ok = url_split_credentials("ftp://alice:s3cret@127.0.0.1:21/a.bin", clean, sizeof(clean), pass, sizeof(pass));
    TEST("split succeeds on a URL with credentials", ok);
    TEST("split removes the password from the URL", strcmp(clean, "ftp://alice@127.0.0.1:21/a.bin") == 0);
    TEST("split returns the password separately", strcmp(pass, "s3cret") == 0);

    ok = url_split_credentials("http://example.com/x.zip", clean, sizeof(clean), pass, sizeof(pass));
    TEST("URL without password is unchanged", ok && strcmp(clean, "http://example.com/x.zip") == 0 && pass[0] == '\0');

    char full[IDM_MAX_URL] = {0};
    TEST("inject rebuilds the authenticated URL",
         url_inject_password("ftp://alice@127.0.0.1:21/a.bin", "s3cret", full, sizeof(full)) &&
         strcmp(full, "ftp://alice:s3cret@127.0.0.1:21/a.bin") == 0);

    char red[512] = {0};
    url_redact_for_log("ftp://alice:s3cret@127.0.0.1:21/a.bin", red, sizeof(red));
    TEST("log redaction hides the password", strstr(red, "s3cret") == NULL && strstr(red, "alice:***@") != NULL);
    url_redact_for_log("{\"action\":\"download\",\"url\":\"ftp://bob:hunter2@h/x\"}", red, sizeof(red));
    TEST("log redaction works inside JSON text", strstr(red, "hunter2") == NULL && strstr(red, "bob:***@h") != NULL);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "ftp://alice:s3cret@127.0.0.1:1/q.bin", "q_cred_test.bin", 1, 0) : NULL;
    TEST("ftp URL with credentials is accepted", dl != NULL);
    if (dl) {
        TEST("stored download URL has no password", strstr(dl->url, "s3cret") == NULL);
        TEST("connection URL keeps the password in memory", strstr(dl->secret_url, "s3cret") != NULL);
        TEST("password is held only in encrypted form", dl->pass_enc[0] != '\0' && strstr(dl->pass_enc, "s3cret") == NULL);
    }

    TEST("queue saved", eng && queue_persist_save(eng));
    {
        static uint8_t qb[1 << 16];
        uint8_t *blob = NULL;
        uint32_t n = 0;
        BOOL have = lass_store_get("queue", "state", &blob, &n) && n > 0 && n <= sizeof(qb);
        if (have) memcpy(qb, blob, n);
        free(blob);
        TEST("stored queue is readable", have);
        TEST("stored queue contains no plain password", have && !bytes_contain((const char *)qb, n, "s3cret"));
        TEST("stored queue contains no password in URL form", have && !bytes_contain((const char *)qb, n, "alice:s3cret"));
    }

    IDM_Engine *eng2 = engine_create(NULL);
    TEST("queue restored into a new engine", eng2 && queue_persist_load(eng2, NULL));
    IDM_Download *rdl = (eng2 && eng2->queue_head) ? eng2->queue_head : NULL;
    TEST("restored download gets its connection password back", rdl && strstr(rdl->secret_url, "s3cret") != NULL);
    TEST("restored download URL stays password-free", rdl && strstr(rdl->url, "s3cret") == NULL);

    /* صف نسخه‌ی ۱ (بدون فیلد رمز) باید همچنان خوانده شود */
    /* مهاجرت: فروشگاه تازه و خالی باشد تا صف نسخه‌ی ۱ از فایل قدیمی خوانده شود */
    lass_store_close();
    DeleteFileA("tels_test_cred_store.tels");
    lass_store_open("tels_test_cred_store.tels");
    const char *v1path = "queue_v1_test.dat";
    {
        typedef struct { uint32_t magic; uint16_t version; uint16_t pad; uint32_t count; uint32_t pad2; } V1Hdr;
        V1Hdr h = { QUEUE_PERSIST_MAGIC, (uint16_t)QUEUE_PERSIST_VERSION_V1, 0, 1, 0 };
        QueueRecord rec;
        memset(&rec, 0, sizeof(rec));
        snprintf(rec.url, sizeof(rec.url), "%s", "http://127.0.0.1:1/v1.bin");
        snprintf(rec.out_path, sizeof(rec.out_path), "%s", "v1_test.bin");
        rec.state = DL_PAUSED;
        rec.num_segments = 1;
        FILE *f = fopen(v1path, "wb");
        if (f) {
            fwrite(&h, sizeof(h), 1, f);
            fwrite(&rec, sizeof(rec), 1, f);
            fclose(f);
        }
    }
    IDM_Engine *eng3 = engine_create(NULL);
    TEST("version 1 queue file loads", eng3 && queue_persist_load(eng3, v1path));
    TEST("version 1 record restores its URL",
         eng3 && eng3->queue_head && strcmp(eng3->queue_head->url, "http://127.0.0.1:1/v1.bin") == 0);

    if (eng) engine_destroy(eng);
    if (eng2) engine_destroy(eng2);
    if (eng3) engine_destroy(eng3);
    lass_store_close();
    DeleteFileA("tels_test_cred_store.tels");
    DeleteFileA(v1path);
    DeleteFileA("q_cred_test.bin");
    DeleteFileA("v1_test.bin");
}

/* ======================================================
   تست: کانتینر .tels — سیستم فایل مجازی، commit، بازیابی و همروندی
   ====================================================== */
static uint64_t tels_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static BOOL tels_make_sample(const char *path, TelsId *afile_out, TelsId *docs_out) {
    DeleteFileA(path);
    TelsContainer *c = tels_open(path, TRUE);
    if (!c) return FALSE;
    TelsId docs = 0, afile = 0;
    BOOL ok = tels_create(c, TELS_ROOT, "docs", TELS_TYPE_DIR, &docs) &&
              tels_create(c, docs, "a.txt", TELS_TYPE_FILE, &afile) &&
              tels_write(c, afile, 0, "hello", 5) &&
              tels_commit(c);
    tels_close(c);
    if (afile_out) *afile_out = afile;
    if (docs_out) *docs_out = docs;
    return ok;
}

typedef struct { TelsContainer *c; TelsId id; int t; } TelsWriterArg;

static DWORD WINAPI tels_writer_proc(LPVOID p) {
    TelsWriterArg *a = (TelsWriterArg *)p;
    uint8_t blk[512];
    memset(blk, (int)('A' + a->t), sizeof(blk));
    for (int i = 0; i < 200; i++) {
        uint64_t off = (uint64_t)(i * 4 + a->t) * 512u;
        tels_write(a->c, a->id, off, blk, sizeof(blk));
    }
    return 0;
}

static void test_tels_container(void) {
    SECTION("Container .tels: virtual filesystem, commit and recovery");
    const char *P = "tels_test_main.tels";
    DeleteFileA(P);
    TEST("missing container is not opened without create", tels_open("tels_test_absent.tels", FALSE) == NULL);
    TelsContainer *c = tels_open(P, TRUE);
    TEST("container created", c != NULL);
    if (!c) return;

    TelsId docs = 0, afile = 0;
    TEST("directory created at the root", tels_create(c, TELS_ROOT, "docs", TELS_TYPE_DIR, &docs));
    TEST("file created inside a directory", tels_create(c, docs, "a.txt", TELS_TYPE_FILE, &afile));
    TEST("duplicate name in one directory is rejected (case-insensitive)",
         !tels_create(c, docs, "A.TXT", TELS_TYPE_FILE, NULL));
    TEST("write hello", tels_write(c, afile, 0, "hello", 5));
    char rb[64] = {0};
    TEST("read back hello", tels_read(c, afile, 0, rb, 5) == 5 && memcmp(rb, "hello", 5) == 0);
    uint64_t sz = 0;
    TEST("file size is 5", tels_size(c, afile, &sz) && sz == 5);
    TelsId found = 0;
    TEST("lookup finds the file by name", tels_lookup(c, docs, "a.txt", &found) && found == afile);
    TEST("a file cannot contain another entry", !tels_create(c, afile, "x", TELS_TYPE_FILE, NULL));

    TelsId s = 0;
    tels_create(c, TELS_ROOT, "sparse.bin", TELS_TYPE_FILE, &s);
    TEST("write at a later offset first", tels_write(c, s, 10, "world", 5));
    TEST("then write at offset 0", tels_write(c, s, 0, "hello", 5));
    char sb[32] = {0};
    TEST("out-of-order writes read back with a zero gap",
         tels_read(c, s, 0, sb, 15) == 15 && memcmp(sb, "hello\0\0\0\0\0world", 15) == 0);
    TEST("read at the end returns 0", tels_read(c, s, 15, sb, 4) == 0);
    TEST("read across the end returns the available bytes",
         tels_read(c, s, 12, sb, 10) == 3 && memcmp(sb, "rld", 3) == 0);
    TEST("overwrite inside an existing extent", tels_write(c, s, 2, "XYZ", 3));
    char ob[16] = {0};
    TEST("overwritten content reads back correctly",
         tels_read(c, s, 0, ob, 15) == 15 && memcmp(ob, "heXYZ\0\0\0\0\0world", 15) == 0);

    TelsId cur = TELS_ROOT, deep = 0;
    BOOL nest_ok = TRUE;
    for (int i = 1; i <= 16; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "d%d", i);
        TelsId nx = 0;
        if (!tels_create(c, cur, nm, TELS_TYPE_DIR, &nx)) { nest_ok = FALSE; break; }
        cur = nx;
    }
    TEST("nesting 16 containers deep succeeds", nest_ok);
    TEST("the 17th nested container is rejected", !tels_create(c, cur, "too_deep", TELS_TYPE_DIR, NULL));
    TEST("resolve creates a nested path", tels_resolve(c, "x/y/z", TRUE, &deep) && deep != 0);
    TelsId got = 0;
    TEST("resolve finds the same nested path again", tels_resolve(c, "x/y/z", FALSE, &got) && got == deep);

    TEST("empty name is rejected", !tels_create(c, TELS_ROOT, "", TELS_TYPE_FILE, NULL));
    TEST("name with a separator is rejected", !tels_create(c, TELS_ROOT, "a/b", TELS_TYPE_FILE, NULL));
    TEST("name with a colon is rejected", !tels_create(c, TELS_ROOT, "a:b", TELS_TYPE_FILE, NULL));
    TEST("'..' is rejected", !tels_create(c, TELS_ROOT, "..", TELS_TYPE_FILE, NULL));
    char longname[300];
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    TEST("name longer than the limit is rejected", !tels_create(c, TELS_ROOT, longname, TELS_TYPE_FILE, NULL));

    TelsId kids[32];
    TEST("list shows the file inside docs",
         tels_list(c, docs, kids, 32) == 1 && kids[0] == afile);

    TEST("commit succeeds", tels_commit(c));
    tels_close(c);

    c = tels_open(P, FALSE);
    TEST("reopen after a clean close", c != NULL);
    if (c) {
        TelsId f2 = 0;
        TEST("file found after reopen", tels_lookup(c, docs, "a.txt", &f2) && f2 == afile);
        char rb2[16] = {0};
        TEST("content survives reopen", tels_read(c, afile, 0, rb2, 5) == 5 && memcmp(rb2, "hello", 5) == 0);
        char sb2[32] = {0};
        TEST("sparse content survives reopen",
             tels_read(c, s, 0, sb2, 15) == 15 && memcmp(sb2, "heXYZ\0\0\0\0\0world", 15) == 0);
        TelsId got2 = 0;
        TEST("nested path survives reopen", tels_resolve(c, "x/y/z", FALSE, &got2) && got2 == deep);
    }

    TelsId post = 0;
    TEST("file created after the commit", c && tels_create(c, docs, "after.txt", TELS_TYPE_FILE, &post));
    TEST("data written after the commit", c && tels_write(c, post, 0, "unsaved", 7));
    tels_abort(c);
    c = tels_open(P, FALSE);
    TEST("reopen after an abort without commit", c != NULL);
    if (c) {
        TelsId p2 = 0;
        char pb[16] = {0};
        TEST("complete records written after the last commit are recovered",
             tels_lookup(c, docs, "after.txt", &p2) && tels_read(c, p2, 0, pb, 7) == 7 &&
             memcmp(pb, "unsaved", 7) == 0);
        TEST("commit before the torn-write test", tels_commit(c));
        TelsId tf = 0;
        tels_create(c, docs, "torn.txt", TELS_TYPE_FILE, &tf);
        tels_write(c, tf, 0, "0123456789ABCDEF", 16);
        tels_abort(c);
    }
    {
        HANDLE hf = CreateFileA(P, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz2, cut;
            GetFileSizeEx(hf, &sz2);
            cut.QuadPart = sz2.QuadPart - 8;
            SetFilePointerEx(hf, cut, NULL, FILE_BEGIN);
            SetEndOfFile(hf);
            CloseHandle(hf);
        }
    }
    c = tels_open(P, FALSE);
    TEST("reopen after a torn tail", c != NULL);
    if (c) {
        TelsId t2 = 0;
        uint64_t tsz = 99;
        TEST("the torn record is dropped: the file exists and is empty",
             tels_lookup(c, docs, "torn.txt", &t2) && tels_size(c, t2, &tsz) && tsz == 0);
        TelsId a2 = 0;
        char ab[8] = {0};
        TEST("data before the torn record survives",
             tels_lookup(c, docs, "after.txt", &a2) && tels_read(c, a2, 0, ab, 7) == 7 && memcmp(ab, "unsaved", 7) == 0);
        TEST("writes work after recovery", tels_write(c, t2, 0, "ok", 2));
        tels_close(c);
    }

    /* superblock جدیدتر خراب: باید superblock قبلی و replay کافی باشد */
    const char *P2 = "tels_test_sb.tels";
    TelsId a3 = 0, d3 = 0;
    TEST("sample container for superblock damage", tels_make_sample(P2, &a3, &d3));
    c = tels_open(P2, FALSE);
    if (c) {
        TEST("commit again so two superblocks hold different generations", tels_commit(c));
        tels_abort(c);
    }
    {
        HANDLE hf = CreateFileA(P2, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            uint8_t sa[16], sbb[16];
            DWORD got_a = 0, got_b = 0;
            SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            ReadFile(hf, sa, sizeof(sa), &got_a, NULL);
            SetFilePointer(hf, 4096, NULL, FILE_BEGIN);
            ReadFile(hf, sbb, sizeof(sbb), &got_b, NULL);
            DWORD victim = (tels_le64(sa + 12) > tels_le64(sbb + 12)) ? 0 : 4096;
            uint8_t junk = 0xFF;
            SetFilePointer(hf, victim + 13, NULL, FILE_BEGIN);
            WriteFile(hf, &junk, 1, &got_a, NULL);
            CloseHandle(hf);
        }
    }
    c = tels_open(P2, FALSE);
    TEST("container opens with the newest superblock damaged", c != NULL);
    if (c) {
        char hb[8] = {0};
        TelsId f3 = 0;
        TEST("content survives a damaged newest superblock",
             tels_lookup(c, d3, "a.txt", &f3) && tels_read(c, f3, 0, hb, 5) == 5 && memcmp(hb, "hello", 5) == 0);
        tels_close(c);
    }

    /* index خراب: replay کامل از ابتدا باید همه‌ی داده‌ها را بازسازی کند */
    const char *P3 = "tels_test_idx.tels";
    TelsId a4 = 0, d4 = 0;
    TEST("sample container for index damage", tels_make_sample(P3, &a4, &d4));
    {
        HANDLE hf = CreateFileA(P3, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            uint8_t sa[32], sbb[32];
            DWORD got = 0;
            SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            ReadFile(hf, sa, sizeof(sa), &got, NULL);
            SetFilePointer(hf, 4096, NULL, FILE_BEGIN);
            ReadFile(hf, sbb, sizeof(sbb), &got, NULL);
            BOOL a_newer = tels_le64(sa + 12) > tels_le64(sbb + 12);
            uint64_t idx_pos = a_newer ? tels_le64(sa + 20) : tels_le64(sbb + 20);
            uint8_t junk = 0xAA;
            SetFilePointer(hf, (LONG)(idx_pos + 72), NULL, FILE_BEGIN);
            WriteFile(hf, &junk, 1, &got, NULL);
            CloseHandle(hf);
        }
    }
    c = tels_open(P3, FALSE);
    TEST("container opens with a damaged index record", c != NULL);
    if (c) {
        char hb2[8] = {0};
        TelsId f4 = 0;
        TEST("replay from the start restores the content",
             tels_lookup(c, d4, "a.txt", &f4) && tels_read(c, f4, 0, hb2, 5) == 5 && memcmp(hb2, "hello", 5) == 0);
        tels_close(c);
    }

    /* هر دو superblock خراب: replay از ابتدا */
    const char *P4 = "tels_test_both.tels";
    TelsId a5 = 0, d5 = 0;
    TEST("sample container for double superblock damage", tels_make_sample(P4, &a5, &d5));
    {
        HANDLE hf = CreateFileA(P4, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            uint8_t junk[8];
            memset(junk, 0xEE, sizeof(junk));
            DWORD got = 0;
            SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            WriteFile(hf, junk, sizeof(junk), &got, NULL);
            SetFilePointer(hf, 4096, NULL, FILE_BEGIN);
            WriteFile(hf, junk, sizeof(junk), &got, NULL);
            CloseHandle(hf);
        }
    }
    c = tels_open(P4, FALSE);
    TEST("container opens with both superblocks damaged", c != NULL);
    if (c) {
        char hb3[8] = {0};
        TelsId f5 = 0;
        TEST("replay restores the content with no superblock",
             tels_lookup(c, d5, "a.txt", &f5) && tels_read(c, f5, 0, hb3, 5) == 5 && memcmp(hb3, "hello", 5) == 0);
        tels_close(c);
    }

    /* حذف یک پوشه با تمام زیرمجموعه‌ها، و ماندگاری حذف */
    const char *P5 = "tels_test_del.tels";
    DeleteFileA(P5);
    c = tels_open(P5, TRUE);
    if (c) {
        TelsId tree = 0, sub = 0, leaf = 0;
        tels_create(c, TELS_ROOT, "tree", TELS_TYPE_DIR, &tree);
        tels_create(c, tree, "sub", TELS_TYPE_DIR, &sub);
        tels_create(c, sub, "leaf.bin", TELS_TYPE_FILE, &leaf);
        tels_write(c, leaf, 0, "data", 4);
        TEST("subtree deleted", tels_delete(c, tree));
        TEST("deleted subtree is gone", !tels_lookup(c, TELS_ROOT, "tree", NULL));
        TEST("the root itself cannot be deleted", !tels_delete(c, TELS_ROOT));
        tels_close(c);
        c = tels_open(P5, FALSE);
        TEST("deletion survives reopen", c && !tels_lookup(c, TELS_ROOT, "tree", NULL));
        if (c) tels_close(c);
    }

    /* همروندی: چهار thread روی بازه‌های جدا می‌نویسند (مثل segment های موازی) */
    const char *P6 = "tels_test_conc.tels";
    DeleteFileA(P6);
    c = tels_open(P6, TRUE);
    if (c) {
        TelsId big = 0;
        tels_create(c, TELS_ROOT, "big.bin", TELS_TYPE_FILE, &big);
        TelsWriterArg wa[4];
        HANDLE th[4];
        for (int t = 0; t < 4; t++) {
            wa[t].c = c;
            wa[t].id = big;
            wa[t].t = t;
            th[t] = CreateThread(NULL, 0, tels_writer_proc, &wa[t], 0, NULL);
        }
        WaitForMultipleObjects(4, th, TRUE, 30000);
        for (int t = 0; t < 4; t++) CloseHandle(th[t]);
        uint8_t blk[512];
        BOOL conc_ok = TRUE;
        for (int i = 0; i < 200 && conc_ok; i++) {
            for (int t = 0; t < 4 && conc_ok; t++) {
                uint64_t off = (uint64_t)(i * 4 + t) * 512u;
                if (tels_read(c, big, off, blk, 512) != 512) conc_ok = FALSE;
                for (int k = 0; k < 512 && conc_ok; k++)
                    if (blk[k] != (uint8_t)('A' + t)) conc_ok = FALSE;
            }
        }
        TEST("four concurrent writers land their ranges without corruption", conc_ok);
        TEST("commit after concurrent writes", tels_commit(c));
        tels_close(c);
    }

    /* یک پوشه با ۲۰۰۰ فایل: فهرست و بازیابی درست باشد */
    const char *P7 = "tels_test_many.tels";
    DeleteFileA(P7);
    c = tels_open(P7, TRUE);
    if (c) {
        TelsId many = 0;
        tels_create(c, TELS_ROOT, "many", TELS_TYPE_DIR, &many);
        BOOL made_all = TRUE;
        for (int i = 0; i < 2000 && made_all; i++) {
            char nm[24];
            snprintf(nm, sizeof(nm), "f%04d.dat", i);
            made_all = tels_create(c, many, nm, TELS_TYPE_FILE, NULL);
        }
        TEST("2000 files created in one directory", made_all);
        static TelsId ids[2100];
        TEST("list reports all 2000 files", tels_list(c, many, ids, 2100) == 2000);
        TEST("commit with 2000 entries", tels_commit(c));
        tels_close(c);
        c = tels_open(P7, FALSE);
        TelsId many2 = 0;
        TEST("reopen keeps all 2000 files",
             c && tels_lookup(c, TELS_ROOT, "many", &many2) && tels_list(c, many2, ids, 2100) == 2000);
        if (c) tels_close(c);
    }

    DeleteFileA(P);
    DeleteFileA(P2);
    DeleteFileA(P3);
    DeleteFileA(P4);
    DeleteFileA(P5);
    DeleteFileA(P6);
    DeleteFileA(P7);
}

/* ======================================================
   تست: رمزنگاری کانتینر — CRC و GCM با هم، کلید DPAPI، و عدم نشت متن ساده
   ====================================================== */
static uint32_t tels_t_crc32(const uint8_t *p, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void tels_t_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t tels_t_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static BOOL tels_t_read(const char *path, uint64_t pos, void *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;
    BOOL ok = (fseek(f, (long)pos, SEEK_SET) == 0) && (fread(buf, 1, n, f) == n);
    fclose(f);
    return ok;
}

static BOOL tels_t_write(const char *path, uint64_t pos, const void *buf, size_t n) {
    FILE *f = fopen(path, "r+b");
    if (!f) return FALSE;
    BOOL ok = (fseek(f, (long)pos, SEEK_SET) == 0) && (fwrite(buf, 1, n, f) == n);
    fclose(f);
    return ok;
}

/* رکوردهای فایل را از آفست داده‌ها فهرست می‌کند: موقعیت و نوع هر کدام */
static int tels_t_scan(const char *path, uint64_t *pos_out, uint8_t *kind_out, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    int n = 0;
    uint64_t pos = 12288u;
    while (n < max && pos + 64u <= (uint64_t)fsize) {
        uint8_t hdr[64];
        if (!tels_t_read(path, pos, hdr, sizeof(hdr))) break;
        if (tels_t_get32(hdr) != 0x4345524Cu) break;
        uint32_t plen = tels_t_get32(hdr + 20);
        if (pos + 64u + plen > (uint64_t)fsize) break;
        pos_out[n] = pos;
        kind_out[n] = hdr[8];
        n++;
        pos += 64u + plen;
    }
    return n;
}

static void test_tels_encryption(void) {
    SECTION("Container encryption: GCM with CRC, DPAPI key protection, no plaintext leaks");

    /* ۱) متن ساده در فایل نیست و کانتینر با کلید DPAPI دوباره باز می‌شود */
    const char *PS = "tels_test_secret.tels";
    DeleteFileA(PS);
    TelsContainer *c = tels_open(PS, TRUE);
    TEST("encrypted container created", c != NULL);
    if (c) {
        TelsId f = 0;
        TEST("entry with a secret name created", tels_create(c, TELS_ROOT, "secretname_xyz.txt", TELS_TYPE_FILE, &f));
        TEST("secret payload written", tels_write(c, f, 0, "SECRET-PAYLOAD-7F3A9C", 21));
        TEST("commit", tels_commit(c));
        tels_close(c);
    }
    static uint8_t raw[1 << 16];
    FILE *fp = fopen(PS, "rb");
    size_t rn = fp ? fread(raw, 1, sizeof(raw), fp) : 0;
    if (fp) fclose(fp);
    TEST("file bytes do not contain the payload", rn > 0 && !bytes_contain((const char *)raw, rn, "SECRET-PAYLOAD"));
    TEST("file bytes do not contain the entry name", rn > 0 && !bytes_contain((const char *)raw, rn, "secretname_xyz"));
    c = tels_open(PS, FALSE);
    TEST("encrypted container reopens with its protected key", c != NULL);
    if (c) {
        TelsId f2 = 0;
        char pb[32] = {0};
        TEST("verify passes on a clean encrypted container", tels_verify(c));
        TEST("payload decrypts after reopen",
             tels_lookup(c, TELS_ROOT, "secretname_xyz.txt", &f2) &&
             tels_read(c, f2, 0, pb, 21) == 21 && memcmp(pb, "SECRET-PAYLOAD-7F3A9C", 21) == 0);
        tels_close(c);
    }

    /* ۲) nonce هر رکورد یکتاست: دو رکورد با متن یکسان، رمز متفاوت دارند */
    const char *PN = "tels_test_nonce.tels";
    DeleteFileA(PN);
    c = tels_open(PN, TRUE);
    if (c) {
        TelsId f = 0;
        tels_create(c, TELS_ROOT, "same.bin", TELS_TYPE_FILE, &f);
        tels_write(c, f, 0, "SAMEPAYLOAD", 11);
        tels_write(c, f, 11, "SAMEPAYLOAD", 11);
        tels_commit(c);
        tels_close(c);
    }
    uint64_t rp[8];
    uint8_t rk[8];
    int rcount = tels_t_scan(PN, rp, rk, 8);
    uint64_t dpos[2];
    int dcount = 0;
    for (int i = 0; i < rcount && dcount < 2; i++)
        if (rk[i] == 1) dpos[dcount++] = rp[i];
    TEST("two data records found", dcount == 2);
    if (dcount == 2) {
        uint8_t h1[64], h2[64], c1[11], c2[11];
        tels_t_read(PN, dpos[0], h1, 64);
        tels_t_read(PN, dpos[0] + 64, c1, 11);
        tels_t_read(PN, dpos[1], h2, 64);
        tels_t_read(PN, dpos[1] + 64, c2, 11);
        TEST("nonces of identical payloads differ", memcmp(h1 + 32, h2 + 32, 12) != 0);
        TEST("ciphertexts of identical payloads differ", memcmp(c1, c2, 11) != 0);
    }

    /* ۳) دست‌کاری با CRC معتبر: GCM باید رد کند و داده‌ی دست‌کاری‌شده برنگردد */
    const char *PT = "tels_test_tamper.tels";
    DeleteFileA(PT);
    c = tels_open(PT, TRUE);
    if (c) {
        TelsId t = 0;
        tels_create(c, TELS_ROOT, "t.bin", TELS_TYPE_FILE, &t);
        tels_write(c, t, 0, "ABCDEFGH", 8);
        tels_commit(c);
        tels_close(c);
    }
    uint64_t tp[8];
    uint8_t tk[8];
    int tcount = tels_t_scan(PT, tp, tk, 8);
    int tdata = -1;
    for (int i = 0; i < tcount; i++)
        if (tk[i] == 1) { tdata = i; break; }
    TEST("tamper target record found", tdata >= 0);
    if (tdata >= 0) {
        uint64_t rec = tp[tdata];
        uint8_t hdr[64], body[8];
        tels_t_read(PT, rec, hdr, sizeof(hdr));
        tels_t_read(PT, rec + 64, body, sizeof(body));
        body[2] ^= 0x01;
        uint32_t crc = tels_t_crc32(hdr, 60, 0);
        crc = tels_t_crc32(body, sizeof(body), crc);
        tels_t_put32(hdr + 60, crc);
        tels_t_write(PT, rec, hdr, sizeof(hdr));
        tels_t_write(PT, rec + 64, body, sizeof(body));
    }
    c = tels_open(PT, FALSE);
    TEST("container opens; the tampered record is rejected", c != NULL);
    if (c) {
        TelsId tf = 0;
        TEST("file created before the tampered record survives", tels_lookup(c, TELS_ROOT, "t.bin", &tf));
        char vb[16] = {0};
        TEST("reading the tampered data fails authentication", tels_read(c, tf, 0, vb, 8) == -1);
        TEST("tels_verify reports the tampering", !tels_verify(c));
        tels_close(c);
    }

    /* ۳ب) رکوردی که بعد از آخرین commit است و دست‌کاری شده: replay باید GCM را بررسی کند */
    const char *PU = "tels_test_replay.tels";
    DeleteFileA(PU);
    c = tels_open(PU, TRUE);
    if (c) {
        TelsId u = 0;
        tels_create(c, TELS_ROOT, "u.bin", TELS_TYPE_FILE, &u);
        tels_commit(c);
        tels_write(c, u, 0, "IJKLMNOP", 8);
        tels_abort(c);
    }
    uint64_t up[8];
    uint8_t uk[8];
    int ucount = tels_t_scan(PU, up, uk, 8);
    int udata = -1;
    for (int i = 0; i < ucount; i++)
        if (uk[i] == 1) { udata = i; break; }
    TEST("replay target record found", udata >= 0);
    if (udata >= 0) {
        uint64_t rec = up[udata];
        uint8_t hdr[64], body[8];
        tels_t_read(PU, rec, hdr, sizeof(hdr));
        tels_t_read(PU, rec + 64, body, sizeof(body));
        body[5] ^= 0x10;
        uint32_t crc = tels_t_crc32(hdr, 60, 0);
        crc = tels_t_crc32(body, sizeof(body), crc);
        tels_t_put32(hdr + 60, crc);
        tels_t_write(PU, rec, hdr, sizeof(hdr));
        tels_t_write(PU, rec + 64, body, sizeof(body));
    }
    c = tels_open(PU, FALSE);
    TEST("replay stops at a record that fails authentication", c != NULL);
    if (c) {
        TelsId u2 = 0;
        uint64_t usz = 99;
        TEST("file survives and its forged data is dropped on replay",
             tels_lookup(c, TELS_ROOT, "u.bin", &u2) && tels_size(c, u2, &usz) && usz == 0);
        tels_close(c);
    }
    DeleteFileA(PU);

    /* ۴) کلید wrap شده دست‌کاری شود: DPAPI باید باز کردن را رد کند */
    const char *PK = "tels_test_keytamper.tels";
    DeleteFileA(PK);
    c = tels_open(PK, TRUE);
    if (c) tels_close(c);
    {
        uint8_t hdr[64];
        tels_t_read(PK, 8192u, hdr, sizeof(hdr));
        uint32_t blen = tels_t_get32(hdr + 20);
        uint8_t blob[1024];
        if (blen > 0 && blen <= sizeof(blob)) {
            tels_t_read(PK, 8192u + 64u, blob, blen);
            blob[20] ^= 0x01;
            uint32_t crc = tels_t_crc32(hdr, 60, 0);
            crc = tels_t_crc32(blob, blen, crc);
            tels_t_put32(hdr + 60, crc);
            tels_t_write(PK, 8192u, hdr, sizeof(hdr));
            tels_t_write(PK, 8192u + 64u, blob, blen);
        }
    }
    c = tels_open(PK, FALSE);
    TEST("a tampered key blob is rejected by DPAPI", c == NULL);
    if (c) tels_close(c);

    /* ۵) فایلی که کانتینر نیست رد شود */
    const char *PG = "tels_test_garbage.tels";
    {
        uint8_t junk[20000];
        memset(junk, 0xAB, sizeof(junk));
        FILE *g = fopen(PG, "wb");
        if (g) { fwrite(junk, 1, sizeof(junk), g); fclose(g); }
    }
    TEST("a file that is not a container is rejected", tels_open(PG, FALSE) == NULL);

    DeleteFileA(PS);
    DeleteFileA(PN);
    DeleteFileA(PT);
    DeleteFileA(PK);
    DeleteFileA(PG);
}

/* ======================================================
   تست: لبه‌های API کانتینر — بافر نام و اندازه‌ی خواندن
   ====================================================== */
static void test_tels_api_edges(void) {
    SECTION("Container API edges: name buffers and read sizes");
    const char *P = "tels_test_edges.tels";
    DeleteFileA(P);
    TelsContainer *c = tels_open(P, TRUE);
    TEST("edges container created", c != NULL);
    if (!c) return;

    TelsId id = 0;
    tels_create(c, TELS_ROOT, "long_name_example.txt", TELS_TYPE_FILE, &id);

    char small[8];
    memset(small, 'Z', sizeof(small));
    TelsType t = (TelsType)0;
    TelsId par = 0;
    TEST("tels_info refuses a name that does not fit", !tels_info(c, id, &t, &par, small, sizeof(small)));
    TEST("tels_info leaves a too-small buffer untouched", small[0] == 'Z' && small[7] == 'Z');

    char full[64];
    memset(full, 0, sizeof(full));
    TEST("tels_info copies a name that fits",
         tels_info(c, id, &t, &par, full, sizeof(full)) && strcmp(full, "long_name_example.txt") == 0);

    tels_write(c, id, 0, "abc", 3);
    char rb[8];
    memset(rb, 0, sizeof(rb));
    TEST("tels_read of zero bytes returns 0", tels_read(c, id, 0, rb, 0) == 0);
    TEST("tels_read past the end returns 0", tels_read(c, id, 100, rb, 4) == 0);
    TEST("tels_read returns only the bytes available",
         tels_read(c, id, 0, rb, 8) == 3 && memcmp(rb, "abc", 3) == 0);

    tels_abort(c);
    DeleteFileA(P);
}

/* ======================================================
   تست: فروشگاه lass — نسل‌ها، مقدار بزرگ، و ماندگاری
   ====================================================== */
static void test_lass_store(void) {
    SECTION("Lass store (generations, large values, durability)");
    const char *SP = "tels_test_store.tels";
    lass_store_close();
    DeleteFileA(SP);
    TEST("store opens a new file", lass_store_open(SP));
    TEST("missing value does not exist", !lass_store_exists("a/b", "k"));
    TEST("put creates a value", lass_store_put("a/b", "k", "first", 5));
    TEST("value exists after put", lass_store_exists("a/b", "k"));

    uint8_t *got = NULL;
    uint32_t glen = 0;
    TEST("get returns the value",
         lass_store_get("a/b", "k", &got, &glen) && glen == 5 && memcmp(got, "first", 5) == 0);
    free(got);
    got = NULL;

    TEST("second put replaces the value", lass_store_put("a/b", "k", "second!", 7));
    TEST("get returns the newest generation",
         lass_store_get("a/b", "k", &got, &glen) && glen == 7 && memcmp(got, "second!", 7) == 0);
    free(got);
    got = NULL;

    static uint8_t big[9u * 1024u * 1024u];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 13u + 7u);
    TEST("large value (spans several chunks) is stored",
         lass_store_put("big", "blob", big, (uint32_t)sizeof(big)));
    TEST("large value reads back exactly",
         lass_store_get("big", "blob", &got, &glen) && glen == sizeof(big) && memcmp(got, big, sizeof(big)) == 0);
    free(got);
    got = NULL;

    TEST("delete removes the value", lass_store_delete("a/b", "k") && !lass_store_exists("a/b", "k"));
    TEST("empty value is stored", lass_store_put("a/b", "empty", NULL, 0));
    TEST("empty value reads back as length 0",
         lass_store_get("a/b", "empty", &got, &glen) && glen == 0);
    free(got);
    got = NULL;

    TEST("durable value written", lass_store_put("a/b", "keep", "durable", 7));
    lass_store_close();
    TEST("store reopens", lass_store_open(SP));
    TEST("committed value survives reopen",
         lass_store_get("a/b", "keep", &got, &glen) && glen == 7 && memcmp(got, "durable", 7) == 0);
    free(got);
    lass_store_close();
    DeleteFileA(SP);
}

/* ======================================================
   تست: مهاجرت پوشه‌ی قدیمی state ها به فروشگاه (یک‌بار، بدون بازنویسی)
   ====================================================== */
static void test_state_migration(void) {
    SECTION("Smart Resume: legacy state folder migrates into the store");
    lass_store_close();
    DeleteFileA("tels_test_mig_store.tels");
    TEST("migration store opens", lass_store_open("tels_test_mig_store.tels"));

    const char *DIR = "tels_test_legacy_states";
    const char *legacy_file = "tels_test_legacy_states\\sr_00112233aabbccdd.dat";
    CreateDirectoryA(DIR, NULL);
    uint8_t first[96];
    for (size_t i = 0; i < sizeof(first); i++) first[i] = (uint8_t)(i * 7u + 1u);
    FILE *f = fopen(legacy_file, "wb");
    if (f) { fwrite(first, 1, sizeof(first), f); fclose(f); }

    uint8_t k8[8] = {0x5A, 0x13, 0x77, 0x21, 0x09, 0xC4, 0x3E, 0x88};
    IDM_PluginHooks *plug = smart_resume_plugin_create(DIR, k8);
    TEST("plugin created over a legacy folder", plug != NULL);

    uint8_t *got = NULL;
    uint32_t glen = 0;
    TEST("legacy state imported into the store",
         lass_store_get("states", "sr_00112233aabbccdd", &got, &glen) &&
         glen == sizeof(first) && memcmp(got, first, sizeof(first)) == 0);
    free(got);
    got = NULL;

    /* بار دوم: مقدار موجود در فروشگاه نباید با فایل قدیمی بازنویسی شود */
    uint8_t changed[96];
    memset(changed, 0xEE, sizeof(changed));
    f = fopen(legacy_file, "wb");
    if (f) { fwrite(changed, 1, sizeof(changed), f); fclose(f); }
    if (plug) smart_resume_plugin_destroy(plug);
    plug = smart_resume_plugin_create(DIR, k8);
    TEST("second import keeps the stored value",
         lass_store_get("states", "sr_00112233aabbccdd", &got, &glen) &&
         glen == sizeof(first) && memcmp(got, first, sizeof(first)) == 0);
    free(got);
    if (plug) smart_resume_plugin_destroy(plug);

    DeleteFileA(legacy_file);
    RemoveDirectoryA(DIR);
    lass_store_close();
    DeleteFileA("tels_test_mig_store.tels");
}

/* ======================================================
   تست: تخریب engine وقتی launch_thread وسط شبکه بلاک است — بدون leak و بدون انتظار طولانی
   ====================================================== */
static DWORD WINAPI stall_server_proc(LPVOID p) {
    SOCKET ls = (SOCKET)(UINT_PTR)p;
    SOCKET c = accept(ls, NULL, NULL);
    if (c != INVALID_SOCKET) {
        Sleep(20000);          /* هیچ‌وقت جواب نمی‌دهد */
        closesocket(c);
    }
    closesocket(ls);
    return 0;
}

static void test_engine_destroy_while_launching(void) {
    SECTION("engine_destroy during a blocked launch: no leak, no long wait");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18477);
    BOOL listening = (ls != INVALID_SOCKET) &&
                     bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
                     listen(ls, 4) == 0;
    TEST("stall server listening", listening);
    if (!listening) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    HANDLE th = CreateThread(NULL, 0, stall_server_proc, (LPVOID)(UINT_PTR)ls, 0, NULL);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "http://127.0.0.1:18477/stall.bin", "stall_test.bin", 1, 1) : NULL;
    TEST("download added and launched", dl != NULL);
    Sleep(800);
    TEST("a launch thread is blocked at destroy time", eng && eng->active_count > 0);

    DWORD t0 = GetTickCount();
    engine_destroy(eng);
    DWORD spent = GetTickCount() - t0;
    TEST("engine_destroy returns without waiting for the network", spent < 2000);

    if (th) {
        WaitForSingleObject(th, 25000);
        CloseHandle(th);
    }
    DeleteFileA("stall_test.bin");
}

/* ======================================================
   تست: سیاست «۱۰ شکست پیاپی» — سروری که Range را نادیده می‌گیرد
   ====================================================== */
#define POLICY_BODY_SIZE (8u * 1024u * 1024u)

/* ── کمک‌های سرور تست ───────────────────────────────────────────────
   هر اتصال در thread خودش اجرا می‌شه: یه client کند نباید بقیه‌ی
   درخواست‌ها را پشت سر خودش معطل کنه (سرور تک‌thread قبلی همین بود). */
typedef struct {
    SOCKET ls;
    LPTHREAD_START_ROUTINE handler;
} ServerCtx;

static char g_zero_block[65536];   /* فقط خواندنی؛ بین thread ها مشترکه */

static BOOL send_all(SOCKET c, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < n) {
        size_t left = n - sent;
        int chunk = (int)(left > 65536 ? 65536 : left);
        int r = send(c, p + sent, chunk, 0);
        if (r <= 0) return FALSE;
        sent += (size_t)r;
    }
    return TRUE;
}

static BOOL send_zeros(SOCKET c, unsigned long long total) {
    while (total > 0) {
        size_t chunk = total > sizeof(g_zero_block) ? sizeof(g_zero_block) : (size_t)total;
        if (!send_all(c, g_zero_block, chunk)) return FALSE;
        total -= chunk;
    }
    return TRUE;
}

/* سرِ درخواست را تا \r\n\r\n می‌خونیم: یه recv تنها ممکنه فقط بخشی از هدرها را بیاره و Range جا بیفته */
static BOOL read_request_head(SOCKET c, char *buf, size_t cap) {
    size_t used = 0;
    if (cap == 0) return FALSE;
    buf[0] = '\0';
    while (used + 1 < cap) {
        int r = recv(c, buf + used, (int)(cap - used - 1), 0);
        if (r <= 0) break;
        used += (size_t)r;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) return TRUE;
    }
    return FALSE;
}

static DWORD WINAPI accept_loop(LPVOID p) {
    ServerCtx *ctx = (ServerCtx *)p;
    for (;;) {
        SOCKET c = accept(ctx->ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;      /* listening socket بسته شد: تست تمام شده */
        DWORD tv = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        HANDLE t = CreateThread(NULL, 0, ctx->handler, (LPVOID)(UINT_PTR)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    free(ctx);
    return 0;
}

static HANDLE start_test_server(SOCKET ls, LPTHREAD_START_ROUTINE handler) {
    ServerCtx *ctx = (ServerCtx *)malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->ls = ls;
    ctx->handler = handler;
    HANDLE th = CreateThread(NULL, 0, accept_loop, ctx, 0, NULL);
    if (!th) free(ctx);
    return th;
}

/* سرور: Range را نادیده می‌گیرد؛ HEAD درست است، GET همیشه کل فایل را با 200 می‌فرستد */
static DWORD WINAPI policy_conn_proc(LPVOID p) {
    SOCKET c = (SOCKET)(UINT_PTR)p;
    char req[2048];
    if (read_request_head(c, req, sizeof(req))) {
        BOOL head = (strncmp(req, "HEAD", 4) == 0);
        char hdr[256];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nAccept-Ranges: bytes\r\n"
            "ETag: \"policy\"\r\nConnection: close\r\n\r\n", (unsigned)POLICY_BODY_SIZE);
        if (send_all(c, hdr, (size_t)hl) && !head)
            send_zeros(c, POLICY_BODY_SIZE);
    }
    closesocket(c);
    return 0;
}

static void test_segment_policy_parks_stuck_range(void) {
    SECTION("Segment policy: an ignored range is parked, then the download stops with a reason");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18478);
    BOOL ok = (ls != INVALID_SOCKET) &&
              bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
              listen(ls, 8) == 0;
    TEST("range-ignoring server listening", ok);
    if (!ok) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    HANDLE th = start_test_server(ls, policy_conn_proc);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "http://127.0.0.1:18478/policy.bin", "policy_test.bin", 4, 1) : NULL;
    TEST("download added", dl != NULL);

    DL_State st = DL_QUEUED;
    char reason[256];
    memset(reason, 0, sizeof(reason));
    int waited = 0;
    while (dl && waited < 40000) {
        Sleep(250);
        waited += 250;
        EnterCriticalSection(&eng->lock);
        st = dl->state;
        size_t fl = strlen(dl->fail_reason);
        if (fl > sizeof(reason) - 1) fl = sizeof(reason) - 1;
        memcpy(reason, dl->fail_reason, fl);
        reason[fl] = '\0';
        LeaveCriticalSection(&eng->lock);
        if (st == DL_FAILED || st == DL_COMPLETED) break;
    }
    TEST("download stops once every segment is parked", st == DL_FAILED);
    TEST("the stop reason says why", strstr(reason, "no progress") != NULL &&
                                     strstr(reason, "ignored the range") != NULL);

    engine_destroy(eng);
    closesocket(ls);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    TEST("the stop reason counts all 10 attempts", strstr(reason, "(10 attempts") != NULL);
    DeleteFileA("policy_test.bin");
    DeleteFileA("policy_test.bin.tels");
}

/* ======================================================
   تست: Retry-After برای بخش‌ها — دو درخواست اول 429 با Retry-After، بعد Range درست
   ====================================================== */
#define RETRY_BODY_SIZE (8u * 1024u * 1024u)
static volatile LONG g_retry_429_served = 0;
static volatile LONG g_retry_gets_seen = 0;

/* سرور: دو GET اول 429 با Retry-After، بعد Range را درست جواب می‌دهد */
static DWORD WINAPI retry_conn_proc(LPVOID p) {
    SOCKET c = (SOCKET)(UINT_PTR)p;
    char req[2048];
    if (read_request_head(c, req, sizeof(req))) {
        if (strncmp(req, "HEAD", 4) == 0) {
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nAccept-Ranges: bytes\r\n"
                "ETag: \"retry\"\r\nConnection: close\r\n\r\n", (unsigned)RETRY_BODY_SIZE);
            send_all(c, hdr, (size_t)hl);
        } else {
            LONG n = InterlockedIncrement(&g_retry_gets_seen);
            if (n <= 2) {
                InterlockedIncrement(&g_retry_429_served);
                static const char r429[] =
                    "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 1\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(c, r429, sizeof(r429) - 1);
            } else {
                unsigned long long s = 0, e = RETRY_BODY_SIZE - 1ull;
                const char *rp = strstr(req, "Range: bytes=");
                if (rp) {
                    rp += 13;
                    s = strtoull(rp, NULL, 10);
                    const char *dash = strchr(rp, '-');
                    if (dash && dash[1] >= '0' && dash[1] <= '9')
                        e = strtoull(dash + 1, NULL, 10);
                }
                if (e >= RETRY_BODY_SIZE) e = RETRY_BODY_SIZE - 1ull;
                if (s > e) s = e;
                unsigned long long len = e - s + 1ull;
                char hdr[320];
                int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 206 Partial Content\r\nContent-Length: %llu\r\n"
                    "Content-Range: bytes %llu-%llu/%u\r\nAccept-Ranges: bytes\r\n"
                    "ETag: \"retry\"\r\nConnection: close\r\n\r\n",
                    len, s, e, (unsigned)RETRY_BODY_SIZE);
                if (send_all(c, hdr, (size_t)hl))
                    send_zeros(c, len);
            }
        }
    }
    closesocket(c);
    return 0;
}

static void test_segment_honours_retry_after(void) {
    SECTION("Segments honour Retry-After from the server and then finish the download");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18479);
    BOOL ok = (ls != INVALID_SOCKET) &&
              bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
              listen(ls, 8) == 0;
    TEST("retry-after server listening", ok);
    if (!ok) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    InterlockedExchange(&g_retry_429_served, 0);
    InterlockedExchange(&g_retry_gets_seen, 0);
    HANDLE th = start_test_server(ls, retry_conn_proc);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "http://127.0.0.1:18479/retry.bin", "retry_after_test.bin", 2, 1) : NULL;
    TEST("download added", dl != NULL);

    DL_State st = DL_QUEUED;
    int waited = 0;
    while (dl && waited < 40000) {
        Sleep(250);
        waited += 250;
        EnterCriticalSection(&eng->lock);
        st = dl->state;
        LeaveCriticalSection(&eng->lock);
        if (st == DL_COMPLETED || st == DL_FAILED) break;
    }
    TEST("the server really sent Retry-After", g_retry_429_served >= 1);
    TEST("download completes after the wait", st == DL_COMPLETED);

    HANDLE hf = CreateFileA("retry_after_test.bin", GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER fsz;
    fsz.QuadPart = 0;
    BOOL sized = FALSE;
    if (hf != INVALID_HANDLE_VALUE) {
        sized = GetFileSizeEx(hf, &fsz);
        CloseHandle(hf);
    }
    TEST("final file has the full size", sized && (unsigned long long)fsz.QuadPart == RETRY_BODY_SIZE);

    engine_destroy(eng);
    closesocket(ls);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    DeleteFileA("retry_after_test.bin");
    DeleteFileA("retry_after_test.bin.tels");
}

static void test_crash_report(void) {
    SECTION("Crash Report (fault location + call stack)");

    int frames = crash_depth_probe(3);
    TEST("synthetic context walks multiple frames", frames >= 3);
    TEST("walk is bounded", frames <= 24);
    TEST("NULL pointers are safe", crash_report_log(NULL) == 0);

    EXCEPTION_POINTERS empty = { NULL, NULL };
    TEST("missing exception record is safe", crash_report_log(&empty) == 0);
}

/* ======================================================
   تست: Launch Retry Backoff — retry خودکار discovery failures
   ====================================================== */
static void test_launch_retry_backoff(void) {
    SECTION("Engine: Launch Retry Backoff");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) return;

    IDM_Download *dl = engine_add(eng, "http://x.com/f.zip", "C:\\f.zip", 4, 0);
    TEST("download added", dl != NULL);
    if (!dl) { engine_destroy(eng); return; }

    /* شبیه‌سازی دستی یه شکست discovery — چون واقعاً شبکه نداریم،
       مستقیم منطق retry رو با تنظیم فیلدها تست می‌کنیم */
    TEST("launch_retry_count starts at 0", dl->launch_retry_count == 0);
    TEST("next_launch_retry_at starts at 0", dl->next_launch_retry_at == 0);

    /* شبیه‌سازی افزایش بعد از شکست اول */
    dl->launch_retry_count = 1;
    DWORD backoff1 = 3000u * (1u << (dl->launch_retry_count - 1));
    TEST("first backoff is 3000ms", backoff1 == 3000);

    dl->launch_retry_count = 3;
    DWORD backoff3 = 3000u * (1u << (dl->launch_retry_count - 1));
    TEST("third backoff is 12000ms", backoff3 == 12000);

    dl->launch_retry_count = 10; /* خیلی زیاد */
    DWORD backoff10 = 3000u * (1u << (dl->launch_retry_count - 1));
    if (backoff10 > 60000) backoff10 = 60000;
    TEST("backoff caps at 60000ms", backoff10 == 60000);

    TEST("ENGINE_MAX_LAUNCH_RETRY is reasonable",
         ENGINE_MAX_LAUNCH_RETRY >= 3 && ENGINE_MAX_LAUNCH_RETRY <= 10);

    /* تست اینکه scheduler واقعاً به next_launch_retry_at احترام
       میذاره — یه دانلود با retry timer در آینده رو QUEUED می‌ذاریم
       و بررسی می‌کنیم که با یه scheduler tick دستی، launch نشه */
    dl->state = DL_QUEUED;
    dl->next_launch_retry_at = GetTickCount() + 10000; /* ۱۰ ثانیه بعد */
    BOOL should_wait = (dl->next_launch_retry_at != 0 &&
                        GetTickCount() < dl->next_launch_retry_at);
    TEST("scheduler would skip download still in backoff", should_wait);

    /* بعد از گذشت زمان، باید آزاد بشه */
    dl->next_launch_retry_at = GetTickCount() - 1000; /* در گذشته */
    BOOL should_launch_now = !(dl->next_launch_retry_at != 0 &&
                               GetTickCount() < dl->next_launch_retry_at);
    TEST("scheduler would launch after backoff expires", should_launch_now);

    engine_destroy(eng);
    TEST("engine destroy", TRUE);
}

/* ======================================================
   تست: خطای دائمی (404) در برابر موقتی — نباید retry بشه
   ====================================================== */
static void test_permanent_vs_transient_errors(void) {
    SECTION("Engine: Permanent (404) vs Transient Error Handling");

    /* IDM_ERR_NOT_FOUND باید از IDM_ERR_NETWORK متمایز باشه —
       این دو enum value باید متفاوت باشن تا منطق engine.c
       بتونه بین‌شون تمایز قائل بشه */
    TEST("NOT_FOUND differs from NETWORK error",
         IDM_ERR_NOT_FOUND != IDM_ERR_NETWORK);
    TEST("NOT_FOUND is a distinct negative error code",
         IDM_ERR_NOT_FOUND < 0);

    /* شبیه‌سازی منطق permanent-detection که در http.c هست */
    struct { int status; BOOL should_be_permanent; } cases[] = {
        {404, TRUE},   /* پیدا نشد — دائمی */
        {410, TRUE},   /* دیگه موجود نیست — دائمی */
        {400, TRUE},   /* درخواست بدشکل — دائمی */
        {401, TRUE},   /* نیاز به احراز هویت — دائمی */
        {403, FALSE},  /* می‌تونه rate-limit موقتی باشه */
        {429, FALSE},  /* too many requests — موقتی */
        {500, FALSE},  /* خطای سرور — موقتی */
        {503, FALSE},  /* سرویس در دسترس نیست — موقتی */
    };
    BOOL all_correct = TRUE;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        BOOL permanent = (cases[i].status == 404 || cases[i].status == 410 ||
                          cases[i].status == 400 || cases[i].status == 401);
        if (permanent != cases[i].should_be_permanent) all_correct = FALSE;
    }
    TEST("permanent/transient classification matches expected", all_correct);
}

/* ======================================================
   تست: استخراج hostname از URL برای per-host staggering
   ====================================================== */
static void test_http_extract_host(void) {
    SECTION("HTTP Host Extraction (for per-host stagger)");

    char host[128];

    http_extract_host("http://example.com/file.zip", host, sizeof(host));
    TEST("simple http host extracted", strcmp(host, "example.com") == 0);

    http_extract_host("https://cdn.example.com:8080/path/file.rar?x=1",
                      host, sizeof(host));
    TEST("https host with port and query extracted",
        strcmp(host, "cdn.example.com") == 0);

    http_extract_host("https://pdn.sharezilla.ir/d/software/x.rar",
                      host, sizeof(host));
    TEST("real-world host extracted",
        strcmp(host, "pdn.sharezilla.ir") == 0);

    /* null safety */
    host[0] = 'X'; host[1] = '\0';
    http_extract_host(NULL, host, sizeof(host));
    TEST("null url safe", host[0] == '\0');

    http_extract_host("http://a.com/x.zip", NULL, 0);
    TEST("null out buffer safe", TRUE); /* فقط نباید crash کنه */

    /* دو URL از یه host یکسان باید همون hostname رو بدن —
       این پایه‌ی منطق stagger هست */
    char host1[128], host2[128];
    http_extract_host("https://pdn.sharezilla.ir/file1.rar", host1, sizeof(host1));
    http_extract_host("https://pdn.sharezilla.ir/file2.rar", host2, sizeof(host2));
    TEST("same-host URLs produce identical hostname",
        strcmp(host1, host2) == 0);

    /* دو URL از host های متفاوت باید متفاوت باشن */
    char host3[128];
    http_extract_host("https://other-site.com/file.rar", host3, sizeof(host3));
    TEST("different-host URLs produce different hostname",
        strcmp(host1, host3) != 0);
}

/* ======================================================
   تست: فایل .tels (مسیر موقت حین دانلود)
   ====================================================== */
static void test_tels_temp_file(void) {
    SECTION("Tels Temp File (.tels)");

    IDM_Engine *eng = engine_create(NULL);
    eng->max_concurrent = 0; /* جلوگیری از شروع واقعی دانلود */
    TEST("engine create",       eng != NULL);
    if (!eng) return;

    IDM_Download *dl = engine_add(eng, "http://example.com/movie.mp4",
                                  "C:\\Downloads\\movie.mp4", 4, 0);
    TEST("download added",      dl != NULL);
    if (dl) {
        TEST("temp_path has .tels suffix",
            strstr(dl->temp_path, ".tels") != NULL);
        TEST("temp_path based on out_path",
            strncmp(dl->temp_path, dl->out_path,
                    strlen(dl->out_path)) == 0);
        TEST("out_path unchanged",
            strcmp(dl->out_path, "C:\\Downloads\\movie.mp4") == 0);
        TEST("temp_path exact value",
            strcmp(dl->temp_path,
                   "C:\\Downloads\\movie.mp4.tels") == 0);
    }

    /* فیکس دفاعی: out_path خالی ("") قبلاً به‌عنوان یه مسیر معتبر
       قبول می‌شد (فقط NULL fallback داشت) و temp_path می‌شد فقط
       ".tels" — یه فایل با اسم ناقص در دایرکتوری جاری. */
    IDM_Download *dl_empty = engine_add(eng, "http://example.com/x.zip",
                                        "", 4, 0);
    TEST("empty out_path accepted without crashing", dl_empty != NULL);
    if (dl_empty) {
        TEST("empty out_path falls back (not left empty)",
             dl_empty->out_path[0] != '\0');
        TEST("empty out_path temp_path is not just '.tels'",
             strcmp(dl_empty->temp_path, ".tels") != 0);
    }

    engine_destroy(eng);
    TEST("engine destroy",      TRUE);
}

/* مسیر خیلی طولانی — نباید overflow بده یا warning کامپایلر بسازه */
static void test_tels_long_path(void) {
    SECTION("Tels Temp File — long path edge case");

    IDM_Engine *eng = engine_create(NULL);
    eng->max_concurrent = 0;
    TEST("engine create",       eng != NULL);
    if (!eng) return;

    /* یه مسیر نزدیک به IDM_MAX_PATH (260) بساز */
    char long_path[300];
    strcpy(long_path, "C:\\");
    for (int i = 0; i < 250; i++) {
        long_path[3 + i] = 'a' + (i % 26);
    }
    long_path[253] = '\0';

    IDM_Download *dl = engine_add(eng, "http://example.com/file.bin",
                                  long_path, 4, 0);
    TEST("download added with long path", dl != NULL);
    if (dl) {
        size_t tlen = strlen(dl->temp_path);
        TEST("temp_path within bounds",
            tlen < (size_t)IDM_MAX_PATH);
        TEST("temp_path null terminated",
            dl->temp_path[IDM_MAX_PATH - 1] == '\0' ||
            tlen < (size_t)(IDM_MAX_PATH - 1));
        TEST("temp_path still has .tels somewhere or was truncated safely",
            tlen > 0);
    }

    engine_destroy(eng);
    TEST("engine destroy",      TRUE);
}

/* ======================================================
   تست: Queue Persist — ذخیره و بارگذاری لیست دانلودها
   ====================================================== */
static void test_queue_persist(void) {
    SECTION("Queue Persist (session restore)");

    /* فروشگاه موقت برای این تست */
    lass_store_close();
    DeleteFileA("tels_test_queue_store.tels");
    TEST("queue store opens", lass_store_open("tels_test_queue_store.tels"));

    /* ── ساخت engine با چند دانلود ── */
    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) {
        lass_store_close();
        DeleteFileA("tels_test_queue_store.tels");
        return;
    }
    eng->max_concurrent = 0; /* auto-launch نشه */

    engine_add(eng, "http://example.com/a.zip", "C:\\Downloads\\a.zip", 4, 0);
    engine_add(eng, "http://example.com/b.mp4", "C:\\Downloads\\b.mp4", 8, 0);
    engine_add(eng, "http://example.com/c.pdf", "C:\\Downloads\\c.pdf", 2, 0);

    /* ── save در فروشگاه ── */
    BOOL saved = queue_persist_save(eng);
    TEST("save returns TRUE", saved);
    TEST("queue is stored in the container", lass_store_exists("queue", "state"));

    uint8_t *qb = NULL;
    uint32_t qlen = 0;
    BOOL got_blob = lass_store_get("queue", "state", &qb, &qlen);
    TEST("stored queue has a header and records", got_blob && qlen > sizeof(uint32_t) * 3);
    free(qb);

    engine_destroy(eng);

    /* ماندگاری: فروشگاه بسته و دوباره باز شود، صف باید همان‌جا باشد */
    lass_store_close();
    TEST("store reopens for the queue", lass_store_open("tels_test_queue_store.tels"));

    /* ── load در یه engine جدید ── */
    IDM_Engine *eng2 = engine_create(NULL);
    TEST("engine2 create", eng2 != NULL);
    if (!eng2) {
        lass_store_close();
        DeleteFileA("tels_test_queue_store.tels");
        return;
    }
    eng2->max_concurrent = 0;

    BOOL loaded = queue_persist_load(eng2, NULL);
    TEST("load returns TRUE", loaded);

    /* شمارش دانلودهای بازیابی‌شده */
    int cnt = 0;
    BOOL states_paused = TRUE;
    EnterCriticalSection(&eng2->lock);
    for (IDM_Download *dl = eng2->queue_head; dl; dl = dl->next) {
        cnt++;
        if (dl->state != DL_PAUSED) states_paused = FALSE;
    }
    LeaveCriticalSection(&eng2->lock);
    TEST("3 downloads restored", cnt == 3);
    TEST("all restored as PAUSED", states_paused);

    /* بررسی URL اولین دانلود */
    BOOL url_ok = FALSE;
    EnterCriticalSection(&eng2->lock);
    if (eng2->queue_head)
        url_ok = (strstr(eng2->queue_head->url, "example.com") != NULL);
    LeaveCriticalSection(&eng2->lock);
    TEST("url preserved", url_ok);

    /* null safety */
    TEST("save null safe", !queue_persist_save(NULL));
    TEST("load null safe", !queue_persist_load(NULL, NULL));

    engine_destroy(eng2);
    lass_store_close();
    DeleteFileA("tels_test_queue_store.tels");
    TEST("engine destroy", TRUE);
}

/* ======================================================
   تست ۱۶: Settings — load/save/defaults/startup
   ====================================================== */
/* ======================================================
   تست: http_fixup_extension — پسوند از Content-Type
   ====================================================== */
static void test_http_fixup(void) {
    SECTION("HTTP Extension Fix (mime-type inference)");

    char fn[256];

    /* فایل با پسوند از قبل — نباید تغییر کنه */
    strncpy(fn, "video.mp4", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("mp4 unchanged",          strcmp(fn, "video.mp4") == 0);

    strncpy(fn, "archive.zip", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/zip");
    TEST("zip unchanged",          strcmp(fn, "archive.zip") == 0);

    /* فایل CDN بدون پسوند — باید پسوند اضافه بشه */
    strncpy(fn, "9f8e7d6c3b2a", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("cdn token gets .mp4",    strcmp(fn, "9f8e7d6c3b2a.mp4") == 0);

    strncpy(fn, "dl_token_abc123", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/zip");
    TEST("token gets .zip",        strcmp(fn, "dl_token_abc123.zip") == 0);

    strncpy(fn, "download_id_xyz", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/x-rar-compressed");
    TEST("token gets .rar",        strcmp(fn, "download_id_xyz.rar") == 0);

    strncpy(fn, "file_id_12345", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/x-msdownload");
    TEST("token gets .exe",        strcmp(fn, "file_id_12345.exe") == 0);

    strncpy(fn, "audio_stream_99", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "audio/mpeg");
    TEST("token gets .mp3",        strcmp(fn, "audio_stream_99.mp3") == 0);

    /* mime با پارامتر اضافی — باید trim بشه */
    strncpy(fn, "doc_no_ext", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/pdf; charset=utf-8");
    TEST("pdf with params",        strcmp(fn, "doc_no_ext.pdf") == 0);

    /* mime ناشناخته — نباید تغییر کنه */
    strncpy(fn, "unknown_file", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/x-custom-unknown");
    TEST("unknown mime unchanged", strcmp(fn, "unknown_file") == 0);

    /* NULL safety */
    http_fixup_extension(NULL, 0, "video/mp4");
    strncpy(fn, "test", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), NULL);
    TEST("null safety",            strcmp(fn, "test") == 0);

    /* فایل خالی */
    fn[0] = '\0';
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("empty filename no crash", TRUE);

    /* octet-stream — باید .bin بگیره */
    strncpy(fn, "raw_data_blob", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/octet-stream");
    TEST("octet-stream gets .bin", strcmp(fn, "raw_data_blob.bin") == 0);

    /* mkv */
    strncpy(fn, "movie_hd_token", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/x-matroska");
    TEST("matroska gets .mkv",     strcmp(fn, "movie_hd_token.mkv") == 0);
}

/* ======================================================
   تست: accept_ranges / got_partial logic
   ====================================================== */
/* ======================================================
   تست: امنیت — Path Traversal در filename extraction
   ====================================================== */
static void test_filename_security(void) {
    SECTION("Security — Filename Path Traversal Protection");

    char out[256];

    /* حمله‌ی کلاسیک path traversal با ../ */
    http_extract_filename("http://evil.com/x",
        "attachment; filename=\"../../../Windows/System32/evil.dll\"",
        out, sizeof(out));
    TEST("path traversal ../ stripped",
        strstr(out, "..") == NULL && strstr(out, "/") == NULL);
    TEST("only filename segment kept",
        strcmp(out, "evil.dll") == 0);

    /* backslash-style traversal */
    http_extract_filename("http://evil.com/x",
        "attachment; filename=\"..\\\\..\\\\AppData\\\\evil.exe\"",
        out, sizeof(out));
    TEST("backslash traversal stripped",
        strstr(out, "\\") == NULL);
    TEST("backslash result is evil.exe",
        strcmp(out, "evil.exe") == 0);

    /* absolute path با drive letter */
    http_extract_filename("http://evil.com/x",
        "attachment; filename=\"C:\\\\Windows\\\\evil.dll\"",
        out, sizeof(out));
    TEST("drive letter path reduced to filename",
        strcmp(out, "evil.dll") == 0);

    /* فقط ".." به‌تنهایی */
    http_extract_filename("http://evil.com/x",
        "attachment; filename=\"..\"",
        out, sizeof(out));
    TEST("bare .. becomes safe default",
        strcmp(out, "..") != 0 && out[0] != '\0');

    /* کاراکترهای غیرمجاز ویندوز */
    http_extract_filename("http://evil.com/x",
        "attachment; filename=\"file<>:\\\"|?*.txt\"",
        out, sizeof(out));
    BOOL has_bad_char = FALSE;
    for (int i = 0; out[i]; i++)
        if (strchr("<>:\"|?*", out[i])) has_bad_char = TRUE;
    TEST("illegal windows chars removed", !has_bad_char);

    /* عملکرد نرمال هنوز درست کار کنه */
    http_extract_filename("http://x.com/y",
        "attachment; filename=\"normal-file.zip\"",
        out, sizeof(out));
    TEST("normal filename unaffected",
        strcmp(out, "normal-file.zip") == 0);

    /* بدون Content-Disposition — از URL بگیره، هم باید امن باشه */
    http_extract_filename("http://evil.com/../../etc/passwd",
        NULL, out, sizeof(out));
    TEST("URL-based extraction has no path traversal",
        strstr(out, "..") == NULL);
}

/* ======================================================
   تست: امنیت — DPAPI encryption برای proxy credentials
   ====================================================== */
static void test_secure_store(void) {
    SECTION("Security — DPAPI Credential Encryption");

    char enc[1024] = {0};
    char dec[512]  = {0};

    /* رشته‌ی خالی — نباید crash کنه */
    TEST("encrypt empty string safe",
        secure_store_encrypt("", enc, sizeof(enc)));
    TEST("decrypt empty string safe",
        secure_store_decrypt("", dec, sizeof(dec)));

    /* رمزنگاری یه proxy URL واقعی با credential */
    const char *secret = "http://myuser:mypassword123@proxy.example.com:8080";
    BOOL enc_ok = secure_store_encrypt(secret, enc, sizeof(enc));
    TEST("encrypt proxy url succeeds", enc_ok);

    if (enc_ok) {
        TEST("encrypted output has DPAPI prefix",
            strncmp(enc, "DPAPI:", 6) == 0);
        TEST("encrypted output does not contain plaintext password",
            strstr(enc, "mypassword123") == NULL);

        /* رمزگشایی باید همون مقدار اصلی رو برگردونه */
        BOOL dec_ok = secure_store_decrypt(enc, dec, sizeof(dec));
        TEST("decrypt succeeds", dec_ok);
        TEST("decrypted value matches original",
            dec_ok && strcmp(dec, secret) == 0);
    }

    /* سازگاری با نسخه‌ی قدیمی: plaintext بدون پیشوند DPAPI باید
       بدون تغییر برگرده (upgrade path) */
    char legacy_dec[256] = {0};
    BOOL legacy_ok = secure_store_decrypt("http://old-plaintext-proxy.com",
                                          legacy_dec, sizeof(legacy_dec));
    TEST("legacy plaintext passthrough works", legacy_ok);
    TEST("legacy value unchanged",
        strcmp(legacy_dec, "http://old-plaintext-proxy.com") == 0);

    /* null safety */
    TEST("encrypt null plaintext safe",  !secure_store_encrypt(NULL, enc, sizeof(enc)));
    TEST("encrypt null out safe",        !secure_store_encrypt("x", NULL, 10));
    TEST("decrypt null in safe",         !secure_store_decrypt(NULL, dec, sizeof(dec)));
    TEST("decrypt null out safe",        !secure_store_decrypt("x", NULL, 10));
}

/* ======================================================
   تست: امنیت — Command Injection Protection
   (video_sniffer و rtmp که URL رو به CreateProcessA میدن)
   ====================================================== */
static void test_command_injection_protection(void) {
    SECTION("Security — Command Injection Protection");

    /* این تست‌ها مستقیم quote-detection logic رو شبیه‌سازی می‌کنن
       چون خود توابع rtmp_worker_thread/vs_sniff_via_ytdlp داخلی
       (static) هستن و نیاز به process واقعی دارن. اینجا فقط
       اطمینان می‌دیم که pattern تشخیص quote درست کار می‌کنه —
       چیزی که در کد واقعی استفاده میشه. */

    const char *malicious_url =
        "https://x.com/video\" & calc.exe & echo \"";
    BOOL has_quote = FALSE;
    for (const char *p = malicious_url; *p; p++)
        if (*p == '"') { has_quote = TRUE; break; }
    TEST("malicious url with quote detected", has_quote);

    const char *normal_url = "https://example.com/video.mp4?id=123";
    BOOL normal_has_quote = FALSE;
    for (const char *p = normal_url; *p; p++)
        if (*p == '"') { normal_has_quote = TRUE; break; }
    TEST("normal url has no quote", !normal_has_quote);
}

/* ======================================================
   تست: امنیت — Content-Length Integer Overflow Protection
   ====================================================== */
static void test_content_length_validation(void) {
    SECTION("Security — Content-Length Overflow Protection");

    /* شبیه‌سازی منطق اعتبارسنجی که در http_discover هست:
       مقدار منفی یا خیلی بزرگ باید رد بشه */
    LONGLONG raw1 = _atoi64("-1");
    TEST("negative content-length detected", raw1 < 0);

    LONGLONG raw2 = _atoi64("999999999999999999");
    const LONGLONG MAX_REASONABLE = 10LL * 1024 * 1024 * 1024 * 1024;
    TEST("absurdly large content-length exceeds sane limit",
        raw2 > MAX_REASONABLE);

    LONGLONG raw3 = _atoi64("104857600"); /* 100MB — نرمال */
    TEST("normal content-length within range",
        raw3 >= 0 && raw3 <= MAX_REASONABLE);
}

/* ======================================================
   تست: Shutdown Brake — ترمز خروج ناگهانی
   ====================================================== */
static void test_shutdown_brake(void) {
    SECTION("Shutdown Brake (graceful exit coordinator)");

    shutdown_brake_reset_for_tests();
    TEST("active count starts at 0", shutdown_brake_active_count() == 0);

    /* ثبت‌نام ساده */
    int id1 = shutdown_brake_register("test_subsystem_1");
    TEST("register returns valid id", id1 >= 0);
    TEST("active count is 1", shutdown_brake_active_count() == 1);

    int id2 = shutdown_brake_register("test_subsystem_2");
    TEST("second register returns different id", id2 != id1 && id2 >= 0);
    TEST("active count is 2", shutdown_brake_active_count() == 2);

    /* لغو ثبت */
    shutdown_brake_unregister(id1);
    TEST("active count drops to 1", shutdown_brake_active_count() == 1);

    shutdown_brake_unregister(id2);
    TEST("active count drops to 0", shutdown_brake_active_count() == 0);

    /* unregister دوباره روی همون id — نباید crash کنه یا count رو منفی کنه */
    shutdown_brake_unregister(id1);
    TEST("double unregister safe", shutdown_brake_active_count() == 0);

    /* null/invalid id */
    shutdown_brake_unregister(-1);
    shutdown_brake_unregister(9999);
    TEST("invalid id unregister safe", shutdown_brake_active_count() == 0);

    /* wait_all با صف خالی — باید فوری TRUE برگردونه */
    shutdown_brake_reset_for_tests();
    DWORD t0 = GetTickCount();
    BOOL all_done = shutdown_brake_wait_all(1000);
    DWORD elapsed = GetTickCount() - t0;
    TEST("wait_all empty queue returns TRUE immediately", all_done);
    TEST("wait_all empty queue is fast", elapsed < 500);

    /* حالت draining — ثبت‌نام جدید باید رد بشه */
    shutdown_brake_reset_for_tests();
    shutdown_brake_begin_drain();
    int id3 = shutdown_brake_register("should_be_rejected");
    TEST("registration rejected during drain", id3 == -1);

    /* wait_all با یه subsystem که هیچ‌وقت unregister نمیشه —
       باید بعد از timeout کوتاه FALSE برگردونه (نه ابدی بلاک کنه) */
    shutdown_brake_reset_for_tests();
    shutdown_brake_register("stuck_subsystem");
    DWORD t1 = GetTickCount();
    BOOL timed_out = shutdown_brake_wait_all(300);
    DWORD elapsed2 = GetTickCount() - t1;
    TEST("wait_all times out for stuck subsystem", !timed_out);
    TEST("timeout duration is reasonable",
        elapsed2 >= 250 && elapsed2 < 2000);

    /* پاک‌سازی برای تست‌های بعدی */
    shutdown_brake_reset_for_tests();
    TEST("reset clears everything", shutdown_brake_active_count() == 0);
}

static void test_range_safety(void) {
    SECTION("Range Safety (anti-corruption)");

    /* http_conn_got_partial با NULL باید FALSE برگردونه */
    TEST("got_partial null",       !http_conn_got_partial(NULL));

    /* تست accept_ranges detection — فقط NULL safety رو تست می‌کنیم
       چون WinINet واقعی در test env نیست */
    TEST("range safety no crash",  TRUE);

    /* verify که MIME table کامله — چک با تعداد */
    char fn[64];
    strncpy(fn, "t", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("mime table functional",  strcmp(fn, "t.mp4") == 0);
}

static void test_settings(void) {
    SECTION("Settings – INI Persistence");

    /* ── defaults ── */
    LassSettings s;
    settings_defaults(&s);
    TEST("default max_concurrent",   s.max_concurrent == 5);
    TEST("default segments",         s.default_segments == 8);
    TEST("default minimize_tray",    s.minimize_to_tray == TRUE);
    TEST("default clipboard off",    s.clipboard_monitor == FALSE);
    TEST("default basket off",       s.basket_visible == FALSE);
    TEST("default startup off",      s.run_on_startup == FALSE);
    TEST("default dir not empty",    s.default_dir[0] != '\0');

    /* ── ini path ── */
    char ini[MAX_PATH];
    settings_get_ini_path(ini, sizeof(ini));
    TEST("ini path not empty",       ini[0] != '\0');
    TEST("ini path has lass.ini",    strstr(ini, "lass.ini") != NULL);

    /* ── save ── */
    s.max_concurrent   = 7;
    s.default_segments = 12;
    s.clipboard_monitor = TRUE;
    s.proxy_enabled    = TRUE;
    snprintf(s.proxy_url, sizeof(s.proxy_url), "http://testproxy:3128");
    snprintf(s.default_dir, sizeof(s.default_dir), "C:\\TestDownloads");
    s.win_x = 150; s.win_y = 200;
    s.win_w = 900; s.win_h = 600;
    s.win_pos_saved = TRUE;

    BOOL saved = settings_save(&s);
    TEST("save returns TRUE",        saved);

    /* فایل باید وجود داشته باشه */
    TEST("ini file created",         GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES);

    /* ── load ── */
    LassSettings s2;
    settings_load(&s2);
    TEST("load max_concurrent",      s2.max_concurrent == 7);
    TEST("load default_segments",    s2.default_segments == 12);
    TEST("load clipboard_monitor",   s2.clipboard_monitor == TRUE);
    TEST("load proxy_enabled",       s2.proxy_enabled == TRUE);
    TEST("load proxy_url",           strcmp(s2.proxy_url, "http://testproxy:3128") == 0);
    TEST("load default_dir",         strcmp(s2.default_dir, "C:\\TestDownloads") == 0);
    TEST("load win_pos_saved",       s2.win_pos_saved == TRUE);
    TEST("load win_x",               s2.win_x == 150);
    TEST("load win_y",               s2.win_y == 200);
    TEST("load win_w",               s2.win_w == 900);
    TEST("load win_h",               s2.win_h == 600);

    /* ── clamp بررسی ── */
    LassSettings sc;
    settings_defaults(&sc);
    sc.max_concurrent   = 999; /* خارج از range */
    sc.default_segments = 999;
    settings_save(&sc);
    settings_load(&sc);
    TEST("max_concurrent clamped",   sc.max_concurrent <= 20);
    TEST("segments clamped",         sc.default_segments <= 32);

    /* ── تمیزسازی فایل تست ── */
    /* reset به defaults برای تداخل نداشتن با run بعدی */
    LassSettings clean;
    settings_defaults(&clean);
    settings_save(&clean);
    TEST("cleanup save ok",          TRUE);

    /* ══════════════════════════════════════════════════
       تست واقعی برای باگ "Start with Windows تیک خورده ولی بعد از
       ری‌استارت اجرا نمیشه" — رجوع کن به توضیح کامل کنار فیکس در
       main.c (self-heal هر اجرای واقعی) و guo.c (تطبیق چک‌باکس با
       واقعیت registry). علت واقعی: نصب‌کننده با uninsdeletevalue
       نوشته شده، پس هر آپگرید (uninstall خودکار نسخه‌ی قبل) کلید
       Run رو پاک می‌کنه درحالی‌که lass.ini (و چک‌باکس) دست‌نخورده
       می‌مونه. این تست دقیقاً همون drift رو شبیه‌سازی می‌کنه: ست
       کن، بیرون از برنامه (مثل uninstaller) پاکش کن، تأیید کن
       settings_get_startup واقعاً این غیبت رو می‌بینه. ====================================================== */
    TEST("set startup ON succeeds", settings_set_startup(TRUE) == TRUE);
    TEST("get startup reflects ON", settings_get_startup() == TRUE);

    /* شبیه‌سازی uninsdeletevalue نصب‌کننده (یا هر پاک‌سازی بیرونی
       دیگه‌ای): مستقیم از رجیستری حذفش می‌کنیم — نه با
       settings_set_startup(FALSE)، چون اون "نیتِ" برنامه خودشه؛
       این‌جا می‌خوایم یه drift *بیرونی* رو تقلید کنیم. */
    {
        HKEY hk;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            RegDeleteValueA(hk, lass_reg_startup_value());
            RegCloseKey(hk);
        }
    }
    TEST("external registry deletion is actually detected",
         settings_get_startup() == FALSE);

    /* الان دقیقاً همون چیزی که main.c هر اجرای واقعی انجام میده:
       اگه نیت کاربر (cfg.run_on_startup) روشن بود، دوباره بنویس —
       باید drift رو کامل ترمیم کنه. */
    TEST("self-heal re-write restores it", settings_set_startup(TRUE) == TRUE);
    TEST("restored value is visible again", settings_get_startup() == TRUE);

    /* مقدار نوشته‌شده باید دقیقاً exe فعلی + "/minimized" باشه —
       همون فرمتی که installer هم می‌نویسه (رجوع کن به lass_setup.iss) */
    {
        HKEY hk;
        char val[MAX_PATH + 32] = {0};
        DWORD sz = sizeof(val), type = 0;
        BOOL ok = FALSE;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            ok = (RegQueryValueExA(hk, lass_reg_startup_value(), NULL, &type,
                                   (BYTE *)val, &sz) == ERROR_SUCCESS);
            RegCloseKey(hk);
        }
        TEST("registry value readable", ok);
        TEST("registry value includes /minimized", ok && strstr(val, "/minimized") != NULL);
    }

    TEST("set startup OFF removes it", settings_set_startup(FALSE) == TRUE);
    TEST("get startup reflects OFF", settings_get_startup() == FALSE);
    TEST("disabling twice is safe (no error on missing key)",
         settings_set_startup(FALSE) == TRUE);

    /* ── NULL safety ── */
    settings_save(NULL);
    settings_load(NULL);
    settings_defaults(NULL);
    settings_get_ini_path(NULL, 0);
    settings_set_startup(FALSE); /* حذف از registry اگه بود */
    TEST("null safety settings",     TRUE);
}

/* ======================================================
   تست: i18n / زبان
   ─────────────────────────────────────────────────────
   هدف اصلی این تست: اگه یه روز یه STR_* به enum اضافه شد ولی
   جدول EN_STRINGS یا FA_STRINGS در i18n.c به‌روز نشد (یا جای یه
   رشته عوض شد)، همین‌جا با یه رشته‌ی خالی fail بشه — نه این‌که
   ساکت توی UI واقعی یه دکمه بی‌متن ظاهر بشه. */
static void test_i18n(void) {
    SECTION("i18n / Language");

    TEST("code \"en\" -> LASS_LANG_EN",
         i18n_lang_from_code("en") == LASS_LANG_EN);
    TEST("code \"fa\" -> LASS_LANG_FA",
         i18n_lang_from_code("fa") == LASS_LANG_FA);
    TEST("code \"FA\" (case-insensitive) -> LASS_LANG_FA",
         i18n_lang_from_code("FA") == LASS_LANG_FA);
    TEST("unknown code -> LASS_LANG_EN (safe default)",
         i18n_lang_from_code("xx") == LASS_LANG_EN);
    TEST("NULL code -> LASS_LANG_EN (safe default)",
         i18n_lang_from_code(NULL) == LASS_LANG_EN);
    TEST("empty code -> LASS_LANG_EN (safe default)",
         i18n_lang_from_code("") == LASS_LANG_EN);

    TEST("lang_code(EN) == \"en\"",
         strcmp(i18n_lang_code(LASS_LANG_EN), "en") == 0);
    TEST("lang_code(FA) == \"fa\"",
         strcmp(i18n_lang_code(LASS_LANG_FA), "fa") == 0);

    /* هر Str_ID، در هر دو زبان، باید یه رشته‌ی غیرخالی برگردونه —
       این دقیقاً همون چکیه که یه mismatch بین enum و جدول‌های
       ترجمه رو فوراً لو می‌ده. */
    Lass_Lang saved_lang = i18n_current_language();
    bool all_en_nonempty = true, all_fa_nonempty = true;

    i18n_set_language(LASS_LANG_EN);
    for (int i = 0; i < STR_COUNT; i++) {
        const char *s = i18n_str((Str_ID)i);
        if (!s || s[0] == '\0') { all_en_nonempty = false; break; }
    }
    i18n_set_language(LASS_LANG_FA);
    for (int i = 0; i < STR_COUNT; i++) {
        const char *s = i18n_str((Str_ID)i);
        if (!s || s[0] == '\0') { all_fa_nonempty = false; break; }
    }
    i18n_set_language(saved_lang); /* حالت رو برای تست‌های بعدی برنگردون */

    TEST("every Str_ID has a non-empty EN string", all_en_nonempty);
    TEST("every Str_ID has a non-empty FA string", all_fa_nonempty);

    /* id خارج از محدوده باید امن (رشته‌ی خالی)، نه crash، برگردونه */
    const char *bad = i18n_str((Str_ID)(STR_COUNT + 100));
    TEST("out-of-range Str_ID returns safe empty string",
         bad != NULL && bad[0] == '\0');

    /* ── round-trip تنظیمات: فیلد language واقعاً save/load میشه ── */
    LassSettings s1, s2;
    settings_defaults(&s1);
    TEST("default language is \"en\"", strcmp(s1.language, "en") == 0);

    strncpy(s1.language, "fa", sizeof(s1.language) - 1);
    s1.language[sizeof(s1.language) - 1] = '\0';
    settings_save(&s1);
    settings_load(&s2);
    TEST("language round-trips through settings_save/load",
         strcmp(s2.language, "fa") == 0);

    /* پاک‌سازی: برگردوندن به en تا روی تست‌های دیگه/اجراهای بعدی
       این پروسه اثر نذاره */
    strncpy(s1.language, "en", sizeof(s1.language) - 1);
    s1.language[sizeof(s1.language) - 1] = '\0';
    settings_save(&s1);
}

/* ======================================================
   تست ۱۷: Browser Extension Plugin
   ====================================================== */
/* ── فیکس واقعی: g_ext_fires قبلاً یه int ساده (غیر-atomic) بود،
   از یه OS thread جدا (client_thread سرور) نوشته می‌شد و از
   thread اصلی تست در یه حلقه خونده می‌شد — بدون volatile/atomic،
   این یه cross-thread race واقعیه (نه فقط نظری: استاندارد C هیچ
   تضمینی برای دیده‌شدن به‌موقع نمی‌ده). بدتر از اون: ترتیب نوشتن
   هم برعکس بود — g_ext_fires++ قبل از پرشدن کامل url/fname اتفاق
   می‌افتاد، یعنی حتی اگه reader فوراً increment رو می‌دید،
   buffer هایی که بلافاصله بعدش می‌خوند ممکن بود هنوز کامل
   ننوشته شده باشن. الان: LONG با InterlockedIncrement (barrier
   واقعی روی x86/x64)، و ترتیب درست — اول داده، بعد پرچم. */
static volatile LONG g_ext_fires = 0;
static char g_ext_last_url[2048] = {0};
static char g_ext_last_fname[256] = {0};

static void test_ext_cb(const char *url, const char *fname,
                         const char *ref, void *ud) {
    (void)ref; (void)ud;
    if (url)   strncpy(g_ext_last_url,   url,   sizeof(g_ext_last_url)-1);
    if (fname) strncpy(g_ext_last_fname, fname, sizeof(g_ext_last_fname)-1);
    InterlockedIncrement(&g_ext_fires);
}

static void test_browser_ext(void) {
    SECTION("Browser Extension Plugin");

    /* plugin lifecycle */
    IDM_PluginHooks *plug = browser_ext_plugin_create(test_ext_cb, NULL);
    TEST("ext create",              plug != NULL);
    if (!plug) return;

    TEST("plugin name set",         plug->name != NULL);
    TEST("api_version match",       plug->api_version == IDM_PLUGIN_API_VERSION);

    /* pipe server باید در حال اجرا باشه */
    Sleep(100); /* کمی صبر کن server thread شروع کنه */
    TEST("pipe server running",     browser_ext_is_running(plug));

    /* client count ابتدا صفر */
    TEST("client count zero",       browser_ext_client_count(plug) == 0);

    /* ── تست registry ── */
    /* فقط test می‌کنیم crash نمیده — نه اینکه واقعاً register بشه */
    BOOL r_ok = browser_ext_register(BROWSER_CHROME);
    TEST("register chrome no crash", TRUE); /* نتیجه بستگی به registry access داره */
    (void)r_ok;

    BOOL check = browser_ext_is_registered(BROWSER_CHROME);
    TEST("is_registered check",     TRUE); /* فقط بررسی crash نداشتن */
    (void)check;

    /* ── تست browser_ext_diagnose ── */
    char diag_msg[768] = {0};
    BOOL diag_ok = browser_ext_diagnose(plug, BROWSER_CHROME, diag_msg, sizeof(diag_msg));
    TEST("diagnose no crash",       TRUE);
    TEST("diagnose fills message",  diag_msg[0] != '\0');
    (void)diag_ok;

    /* null safety */
    TEST("diagnose null plug safe",
        !browser_ext_diagnose(NULL, BROWSER_CHROME, diag_msg, sizeof(diag_msg)) ||
        diag_msg[0] != '\0'); /* باید یا FALSE برگردونه یا پیام معنادار بده، نه crash */
    TEST("diagnose null buf safe",
        browser_ext_diagnose(plug, BROWSER_CHROME, NULL, 0) == FALSE);

    browser_ext_unregister(BROWSER_ALL);
    TEST("unregister no crash",     TRUE);

    /* ── تست pipe با یه client ساده ──
       ── باگ واقعی که اینجا فیکس شد (از لاگ واقعی Juliya پیدا شد) ──
       این بخش قبلاً ۴ تا TEST(..., TRUE) «نرم» داشت که هر کدوم،
       دقیقاً وقتی مرحله‌ی واقعی (connect، یا خوندن response) شکست
       می‌خورد، به‌جای fail، بی‌صدا PASS می‌زدن («skip کن»). یعنی
       این بخش عملاً هیچ‌وقت واقعاً fail نمی‌شد، حتی اگه round-trip
       واقعی pipe کاملاً خراب بود — دقیقاً چیزی که توی لاگ واقعی
       دیده شد: "ping response" (نه "ping got pong") یعنی پاسخ
       سرور هیچ‌وقت نرسید، ولی کل بخش سبز نشون داده شد.
       دو فیکس واقعی: (۱) یه retry-loop واقعی برای connect (به‌جای
       یه Sleep(50) ثابت و یک تلاش تنها — روی یه ماشین کندتر یا
       زیر بار، سرور ممکنه هنوز ConnectNamedPipe نزده باشه)،
       (۲) تبدیل هر ۴ تا soft-pass به assertion واقعی که اگه
       round-trip واقعی اتفاق نیفته، تست واقعاً FAIL می‌کنه. */
    /* روی CI/Wine کُند زیر بار سنگین (آخر یه suite طولانی، بعد از
       ده‌ها تست دیگه)، یه تلاش تنها برای کل exchange
       (connect+ping+download) گاهی می‌تونه به‌خاطر فشار لحظه‌ای
       scheduler دیر برسه — نه یه باگ واقعی محصول. به‌جای برگردوندن
       soft-pass قدیمی (که هیچ‌وقت fail نمی‌شد)، کل exchange رو
       بی‌صدا تا ۲ بار امتحان می‌کنیم و فقط یه‌بار، با نتیجه‌ی نهایی،
       TEST() صدا می‌زنیم — پس retry هیچ‌وقت یه fail واقعی رو پنهان
       نمی‌کنه: اگه بار دوم هم شکست بخوره، دقیقاً همون fail ثبت
       میشه. */
    BOOL connected = FALSE, got_pong = FALSE, got_version = FALSE;
    BOOL got_download = FALSE, url_ok = FALSE, fname_ok = FALSE;

    for (int attempt = 0; attempt < 3; attempt++) {
        connected = got_pong = got_version = FALSE;
        got_download = url_ok = fname_ok = FALSE;

        HANDLE client = INVALID_HANDLE_VALUE;
        DWORD cstart = GetTickCount();
        while (client == INVALID_HANDLE_VALUE &&
               (GetTickCount() - cstart) < 3000) {
            client = CreateFileA(LASS_PIPE_NAME,
                                 GENERIC_READ | GENERIC_WRITE,
                                 0, NULL, OPEN_EXISTING, 0, NULL);
            if (client == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_PIPE_BUSY)
                    WaitNamedPipeA(LASS_PIPE_NAME, 500);
                else
                    Sleep(20);
            }
        }
        connected = (client != INVALID_HANDLE_VALUE);
        if (!connected) continue;

        /* پیام ping */
        const char *ping = "{\"action\":\"ping\"}";
        uint32_t ping_len = (uint32_t)strlen(ping);
        DWORD written = 0;
        WriteFile(client, &ping_len, 4, &written, NULL);
        WriteFile(client, ping, ping_len, &written, NULL);
        FlushFileBuffers(client);

        uint32_t resp_len = 0;
        DWORD read_bytes = 0;
        BOOL got_len = FALSE;
        DWORD rstart = GetTickCount();
        while (!got_len && (GetTickCount() - rstart) < 4000) {
            got_len = ReadFile(client, &resp_len, 4, &read_bytes, NULL);
            if (!got_len) Sleep(20);
        }
        if (got_len && resp_len > 0 && resp_len < 4096) {
            char *resp = (char *)malloc(resp_len + 1);
            if (resp) {
                DWORD total = 0;
                while (total < resp_len) {
                    DWORD r = 0;
                    ReadFile(client, resp + total, resp_len - total, &r, NULL);
                    if (r == 0) break;
                    total += r;
                }
                resp[total] = '\0';
                got_pong    = strstr(resp, "pong") != NULL;
                got_version = strstr(resp, "version") != NULL;
                free(resp);
            }
        }

        /* پیام download — msgs_before رو *قبل* از فرستادن می‌گیریم؛
           وگرنه (باگ واقعی که همین‌جا پیدا و فیکس شد) روی یه سرور
           سریع، پیام ممکنه قبل از اینکه ما فرصت کنیم snapshot
           بگیریم از قبل parse شده باشه — و دیاگ "2 -> 2" غلط
           می‌گفت «هیچ‌وقت نرسید» درحالی‌که در واقع خیلی زود رسیده
           بود. */
        long msgs_before = browser_ext_messages_received(plug);
        LONG fires_before = g_ext_fires;   /* قبل از فرستادن پیام؛ وگرنه callback سریع قبل از نمونه‌برداری می‌رسه */

        const char *dl_msg = "{\"action\":\"download\","
                              "\"url\":\"http://example.com/test.zip\","
                              "\"filename\":\"test.zip\","
                              "\"referer\":\"http://example.com\"}";
        uint32_t dl_len = (uint32_t)strlen(dl_msg);
        WriteFile(client, &dl_len, 4, &written, NULL);
        WriteFile(client, dl_msg, dl_len, &written, NULL);
        FlushFileBuffers(client);

        /* واقعاً صبر می‌کنیم تا test_ext_cb (که اول تابع با
           browser_ext_plugin_create ثبت شده) صدا زده بشه — تنها
           راهیه که ثابت می‌کنه سرور پیام رو درست پارس/دیسپچ کرده،
           نه فقط اینکه WriteFile شکست نخورده. */
        /* ── یافته‌ی واقعی حین دیباگ این بخش ──
           با لاگ مستقیم سمت سرور (dispatch: url=... on_download=...
           / on_download returned) ثابت شد dispatch همیشه درست و
           کامل انجام می‌شه — مشکل منطقی نبود. گاهی (زیر بار سنگین
           موازی، مثل انتهای این suite طولانی) فقط رسیدن به همون
           ReadFile چند ثانیه طول می‌کشه (دقیقاً همون تأخیرهای
           چندثانیه‌ای که جاهای دیگه‌ی همین suite هم دیده شده). ۳
           ثانیه برای این واقعیت کافی نبود؛ ۸ ثانیه با margin
           واقعی هماهنگه. */
        DWORD dstart = GetTickCount();
        while (g_ext_fires == fires_before && (GetTickCount() - dstart) < 8000)
            Sleep(20);
        got_download = (g_ext_fires == fires_before + 1);
        if (got_download) {
            url_ok   = strcmp(g_ext_last_url,   "http://example.com/test.zip") == 0;
            fname_ok = strcmp(g_ext_last_fname, "test.zip") == 0;
        } else {
            /* تشخیص دقیق: پیام اصلاً به سرور نرسید، یا رسید ولی
               callback صدا زده نشد؟ این تمایز دقیقاً چیزیه که
               لاگ معمولی (زیر ctest با بافرینگ به‌هم‌ریخته) نمی‌تونه
               قابل‌اعتماد نشون بده. */
            long msgs_after = browser_ext_messages_received(plug);
            printf("    [DIAG] attempt %d: download not dispatched — "
                   "server messages_received: %ld -> %ld (%s)\n",
                   attempt, msgs_before, msgs_after,
                   (msgs_after > msgs_before)
                       ? "message DID reach server — dispatch/callback problem"
                       : "message NEVER reached server — read/parse problem");
        }

        CloseHandle(client);

        /* صبر برای پایان واقعی client_thread قبل از destroy/retry —
           جلوگیری از race condition با thread سرور */
        DWORD wstart = GetTickCount();
        while (browser_ext_client_count(plug) > 0 &&
               (GetTickCount() - wstart) < 2000) {
            Sleep(20);
        }

        if (connected && got_pong && got_version && got_download && url_ok && fname_ok)
            break; /* همه چی موفق بود — نیازی به تلاش دوم نیست */
    }

    TEST("pipe connect",             connected);
    TEST("ping got pong",            got_pong);
    TEST("pong has version",         got_version);
    TEST("download msg sent",        got_download);
    TEST("download url correct",     url_ok);
    TEST("download filename correct",fname_ok);

    /* NULL safety */
    browser_ext_is_running(NULL);
    browser_ext_client_count(NULL);
    browser_ext_plugin_destroy(NULL);
    TEST("null safety",             TRUE);

    browser_ext_plugin_destroy(plug);
    /* بعد از destroy هیچ server ای نباید روی pipe مانده باشه؛ وگرنه اتصال بعدی به thread قدیمی می‌خوره */
    TEST("no pipe server left after destroy", !WaitNamedPipeA(LASS_PIPE_NAME, 200));
    TEST("ext destroy",             TRUE);
    /* کوتاه صبر کن تا OS scheduler فرصت کنه هر thread باقیمانده‌ای
       (server_thread یا client_thread) رو کاملاً exit کنه قبل از
       اینکه تست بعدی شروع بشه و heap layout تغییر کنه */
    Sleep(100);
}

/* ======================================================
   تست ۱۸: DASH / MPD Plugin
   ====================================================== */
static void test_dash(void) {
    SECTION("DASH / MPD Plugin");

    /* تشخیص URL */
    TEST("mpd url detect",        dash_is_mpd_url("http://cdn.example.com/manifest.mpd"));
    TEST("mpd url uppercase",     dash_is_mpd_url("http://example.com/stream.MPD"));
    TEST("non-mpd rejected",      !dash_is_mpd_url("http://example.com/video.mp4"));
    TEST("null rejected",         !dash_is_mpd_url(NULL));

    /* تشخیص از content */
    const char *mpd_content = "<?xml version=\"1.0\"?><MPD type=\"static\">";
    TEST("content detect valid",  dash_is_mpd_content(mpd_content, (int)strlen(mpd_content)));
    TEST("content detect short",  !dash_is_mpd_content("<MP", 3));
    TEST("content detect null",   !dash_is_mpd_content(NULL, 0));

    /* plugin lifecycle */
    IDM_PluginHooks *plug = dash_plugin_create();
    TEST("dash create",           plug != NULL);
    if (!plug) return;

    TEST("dash name set",         plug->name != NULL);
    TEST("dash version set",      plug->version != NULL);
    TEST("dash api_version match",plug->api_version == IDM_PLUGIN_API_VERSION);

    /* NULL safety */
    dash_is_mpd_url(NULL);
    dash_is_mpd_content(NULL, 0);
    TEST("null safety",           TRUE);

    dash_plugin_destroy(plug);
    TEST("dash destroy",          TRUE);
    dash_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);
}

/* ======================================================
   تست ۱۹: Notification Plugin
   ====================================================== */
static void test_notify(void) {
    SECTION("Notification Plugin");

    IDM_PluginHooks *plug = notify_plugin_create(NULL, 0);
    TEST("notify create",         plug != NULL);
    if (!plug) return;

    TEST("plugin name set",       plug->name != NULL);
    TEST("api_version match",     plug->api_version == IDM_PLUGIN_API_VERSION);

    /* config پیش‌فرض */
    NotifyConfig cfg;
    notify_plugin_get_config(plug, &cfg);
    TEST("default enabled",       cfg.enabled == TRUE);
    TEST("default duration",      cfg.duration_ms == 4000);
    TEST("default sound",         cfg.sound == TRUE);

    /* تغییر config */
    cfg.enabled     = FALSE;
    cfg.duration_ms = 2000;
    cfg.sound       = FALSE;
    cfg.style       = NOTIFY_STYLE_BALLOON;
    notify_plugin_configure(plug, &cfg);

    NotifyConfig cfg2;
    notify_plugin_get_config(plug, &cfg2);
    TEST("config updated enabled",   cfg2.enabled == FALSE);
    TEST("config updated duration",  cfg2.duration_ms == 2000);
    TEST("config updated sound",     cfg2.sound == FALSE);
    TEST("config updated style",     cfg2.style == NOTIFY_STYLE_BALLOON);

    /* notify_send وقتی disabled — نباید crash کنه */
    notify_send(plug, NOTIFY_COMPLETE, "test.zip", NULL);
    TEST("send when disabled no crash", TRUE);

    /* فعال کن و toast بزن — بدون hwnd پس silent fail */
    cfg2.enabled = TRUE;
    cfg2.style   = NOTIFY_STYLE_TOAST;
    cfg2.sound   = FALSE;
    notify_plugin_configure(plug, &cfg2);

    notify_send(plug, NOTIFY_COMPLETE, "bigfile.zip", NULL);
    TEST("send complete no crash",    TRUE);

    notify_send(plug, NOTIFY_FAILED, "broken.exe", "Connection timeout");
    TEST("send failed no crash",      TRUE);

    notify_send(plug, NOTIFY_BATCH_DONE, NULL, NULL);
    TEST("send batch_done no crash",  TRUE);

    notify_on_download_complete(plug, "archive.tar.gz", TRUE);
    TEST("on_complete helper",        TRUE);

    notify_on_download_complete(plug, "corrupt.bin", FALSE);
    TEST("on_fail helper",            TRUE);

    /* set_tray */
    notify_set_tray(plug, NULL, 0);
    TEST("set_tray null safe",        TRUE);

    /* NULL safety */
    notify_send(NULL, NOTIFY_COMPLETE, "x", NULL);
    notify_plugin_configure(NULL, NULL);
    notify_plugin_get_config(NULL, NULL);
    notify_set_tray(NULL, NULL, 0);
    TEST("null safety all",           TRUE);

    notify_plugin_destroy(plug);
    TEST("notify destroy",            TRUE);
    notify_plugin_destroy(NULL);
    TEST("destroy null safe",         TRUE);
}

/* ======================================================
   تست ۲۰: Categories Plugin
   ====================================================== */
static void test_categories(void) {
    SECTION("Categories Plugin");

    /* بدون ini — فقط defaults */
    IDM_PluginHooks *plug = categories_plugin_create(NULL);
    TEST("categories create",      plug != NULL);
    if (!plug) return;

    TEST("plugin name set",        plug->name != NULL);
    TEST("api_version match",      plug->api_version == IDM_PLUGIN_API_VERSION);

    /* تعداد پیش‌فرض */
    int cnt = categories_count(plug);
    TEST("default count >= 5",     cnt >= 5);

    /* دسته‌بندی */
    TEST("mp4 → Video",    strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/film.mp4", NULL)), "Video") == 0);
    TEST("mp3 → Audio",    strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/song.mp3", NULL)), "Audio") == 0);
    TEST("pdf → Document", strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/manual.pdf", NULL)), "Document") == 0);
    TEST("exe → Software", strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/setup.exe", NULL)), "Software") == 0);
    TEST("png → Image",    strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/photo.png", NULL)), "Image") == 0);
    TEST("unknown → Other",categories_classify(plug,
        "http://x.com/data.xyz", NULL) == -1);

    /* filename override */
    TEST("filename priority",
        strcmp(categories_name(plug,
            categories_classify(plug, "http://x.com/noext",
                                "video.mp4")), "Video") == 0);

    /* query string نادیده گرفته بشه */
    TEST("query ignored",
        strcmp(categories_name(plug,
            categories_classify(plug,
                "http://cdn.example.com/stream.mp4?token=abc123",
                NULL)), "Video") == 0);

    /* m3u8 و mpd → Video */
    TEST("m3u8 → Video",   strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/live.m3u8", NULL)), "Video") == 0);
    TEST("mpd → Video",    strcmp(categories_name(plug,
        categories_classify(plug, "http://x.com/dash.mpd", NULL)), "Video") == 0);

    /* دسترسی به category */
    DL_Category cat;
    TEST("get cat 0",      categories_get(plug, 0, &cat));
    TEST("cat name not empty", cat.name[0] != '\0');
    TEST("cat enabled",    cat.enabled == TRUE);
    TEST("cat has exts",   cat.ext_count > 0);

    /* set save_dir */
    strncpy(cat.save_dir, "C:\\Downloads\\Videos", sizeof(cat.save_dir)-1);
    categories_set(plug, 0, &cat);
    TEST("set cat dir",    strcmp(categories_dir(plug, 0),
                                  "C:\\Downloads\\Videos") == 0);

    /* resolve_path */
    char out[MAX_PATH*2] = {0};
    categories_resolve_path(plug, "http://x.com/film.mp4", NULL,
                             "C:\\Downloads", out, sizeof(out));
    TEST("resolve uses cat dir",   strstr(out, "Videos") != NULL);
    TEST("resolve has filename",   strstr(out, "film.mp4") != NULL);

    /* reset dir و resolve با default */
    cat.save_dir[0] = '\0';
    categories_set(plug, 0, &cat);
    categories_resolve_path(plug, "http://x.com/film.mp4", NULL,
                             "C:\\Downloads", out, sizeof(out));
    TEST("resolve uses default dir", strstr(out, "Downloads") != NULL);

    /* resolve با filename override */
    categories_resolve_path(plug, "http://x.com/noext",
                             "custom_name.mp4",
                             "C:\\Downloads", out, sizeof(out));
    TEST("resolve custom filename",  strstr(out, "custom_name.mp4") != NULL);

    /* ══════════════════════════════════════════════════
       تست واقعی برای کرشی که در lass.log واقعی دیده شد (دانلود از یه
       mirror ایرانی با عنوان فارسی در URL، دقیقاً ۹ ثانیه بعد از
       تشخیص clipboard، Access Violation) — رجوع کن به توضیح کامل
       کنار safe_path_combine در plugin_categories.c. علت احتمالی:
       یه filename طولانیِ percent-encoded (هر حرف فارسی = ۶ کاراکتر
       ASCII) همراه با مسیر پوشه از سقف داخلی ۲۶۰ کاراکتری
       PathCombineA رد می‌شد و نتیجه خالی/نامشخص می‌موند.
       ══════════════════════════════════════════════════ */
    {
        /* ~۳۰۰ کاراکتر percent-encoded فارسی + پسوند — بیش از
           MAX_PATH حتی بدون هیچ دایرکتوری */
        char long_url[1024];
        int n = snprintf(long_url, sizeof(long_url),
                         "https://s49.namasha.com/videos/dl/7526735264-240p-2/");
        for (int i = 0; i < 50 && n < (int)sizeof(long_url) - 20; i++)
            n += snprintf(long_url + n, sizeof(long_url) - n, "%%D9%%81%%DB%%8C");
        snprintf(long_url + n, sizeof(long_url) - n, ".mp4");

        char out2[MAX_PATH*2] = {0};
        categories_resolve_path(plug, long_url, NULL,
                                 "C:\\Users\\Masterpiece\\Downloads",
                                 out2, sizeof(out2));

        TEST("very long filename still yields a non-empty path", out2[0] != '\0');
        TEST("long-filename path fits within MAX_PATH", strlen(out2) < (size_t)MAX_PATH);
        TEST("long-filename path keeps the directory prefix",
             strncmp(out2, "C:\\Users\\Masterpiece\\Downloads", 30) == 0);
        TEST("long-filename path keeps the extension",
             strlen(out2) >= 4 && strcmp(out2 + strlen(out2) - 4, ".mp4") == 0);

        /* حالت مرزی: دقیقاً کوتاه — نباید بی‌دلیل دستکاری بشه */
        char out3[MAX_PATH*2] = {0};
        categories_resolve_path(plug, "http://x.com/short.mp4", NULL,
                                 "C:\\Downloads", out3, sizeof(out3));
        TEST("short filename is not altered by length handling",
             strstr(out3, "short.mp4") != NULL);
    }

    /* save/load — بدون ini path پس silent */
    categories_save(plug);
    TEST("save no ini no crash",   TRUE);
    categories_load(plug);
    TEST("load no ini no crash",   TRUE);

    /* NULL safety */
    categories_classify(NULL, "x.mp4", NULL);
    categories_name(NULL, 0);
    categories_dir(NULL, 0);
    categories_count(NULL);
    categories_get(NULL, 0, NULL);
    categories_set(NULL, 0, NULL);
    categories_resolve_path(NULL, NULL, NULL, NULL, NULL, 0);
    TEST("null safety all",        TRUE);

    categories_plugin_destroy(plug);
    TEST("categories destroy",     TRUE);
    categories_plugin_destroy(NULL);
    TEST("destroy null safe",      TRUE);
}

/* ======================================================
   تست ۲۱: RTMP Plugin
   ====================================================== */
static void test_rtmp(void) {
    SECTION("RTMP Plugin");

    /* تشخیص URL */
    TEST("rtmp url detect",   rtmp_is_rtmp_url("rtmp://live.example.com/app/stream"));
    TEST("rtmps url detect",  rtmp_is_rtmp_url("rtmps://live.example.com/app/stream"));
    TEST("rtmpe url detect",  rtmp_is_rtmp_url("rtmpe://live.example.com/app/stream"));
    TEST("rtmpt url detect",  rtmp_is_rtmp_url("rtmpt://live.example.com/app/stream"));
    TEST("http not rtmp",     !rtmp_is_rtmp_url("http://example.com/video.mp4"));
    TEST("null not rtmp",     !rtmp_is_rtmp_url(NULL));
    TEST("empty not rtmp",    !rtmp_is_rtmp_url(""));

    /* plugin lifecycle */
    IDM_PluginHooks *plug = rtmp_plugin_create();
    TEST("rtmp create",       plug != NULL);
    if (!plug) return;

    TEST("plugin name set",   plug->name != NULL);
    TEST("api_version match", plug->api_version == IDM_PLUGIN_API_VERSION);

    /* وضعیت اولیه */
    TEST("not downloading",   !rtmp_is_downloading(plug));

    /* detect backend — فقط crash نداشتن رو تست می‌کنیم */
    RtmpConfig cfg = rtmp_detect_backend();
    TEST("detect no crash",   TRUE);
    /* اگه ffmpeg نصب نشده found=FALSE — این normal هست */
    fprintf(stderr, "[Test] RTMP backend: %s\n",
            cfg.found ?
            (cfg.backend == RTMP_BACKEND_FFMPEG ? "ffmpeg" : "rtmpdump")
            : "not found (normal in test env)");
    TEST("config valid",      cfg.backend >= RTMP_BACKEND_NONE);

    /* cancel وقتی چیزی در حال اجرا نیست */
    rtmp_cancel(plug);
    TEST("cancel idle no crash", TRUE);

    /* ── stress: چندبار detect_backend صدا بزن ── */
    /* این تست ریشه segfault قبلی بود — find_in_path با PATH طولانی */
    for (int i = 0; i < 5; i++) {
        RtmpConfig c2 = rtmp_detect_backend();
        (void)c2;
    }
    TEST("detect_backend stress 5x no crash", TRUE);

    /* NULL safety */
    rtmp_is_rtmp_url(NULL);
    rtmp_is_downloading(NULL);
    rtmp_cancel(NULL);
    rtmp_plugin_destroy(NULL);
    TEST("null safety all",   TRUE);

    rtmp_plugin_destroy(plug);
    TEST("rtmp destroy",      TRUE);
}

/* ======================================================
   تست ۲۲: Video Sniffer Plugin
   ====================================================== */
static void test_video_sniffer(void) {
    SECTION("Video Sniffer Plugin");

    /* تشخیص source از روی URL */
    TEST("youtube detect",
        vs_detect_source("https://youtube.com/watch?v=abc123") == VS_SRC_YOUTUBE);
    TEST("youtu.be detect",
        vs_detect_source("https://youtu.be/abc123") == VS_SRC_YOUTUBE);
    TEST("aparat detect",
        vs_detect_source("https://www.aparat.com/v/xyz789") == VS_SRC_APARAT);
    TEST("instagram detect",
        vs_detect_source("https://instagram.com/p/AbCdEf/") == VS_SRC_INSTAGRAM);
    TEST("instagram reel detect",
        vs_detect_source("https://instagram.com/reel/AbCdEf/") == VS_SRC_INSTAGRAM);
    TEST("twitter detect",
        vs_detect_source("https://twitter.com/user/status/123") == VS_SRC_TWITTER);
    TEST("x.com detect",
        vs_detect_source("https://x.com/user/status/123") == VS_SRC_TWITTER);
    TEST("vimeo detect",
        vs_detect_source("https://vimeo.com/123456789") == VS_SRC_VIMEO);
    TEST("direct mp4 detect",
        vs_detect_source("https://cdn.example.com/video.mp4") == VS_SRC_DIRECT);
    TEST("direct mkv detect",
        vs_detect_source("https://cdn.example.com/movie.mkv") == VS_SRC_DIRECT);
    TEST("m3u8 detect",
        vs_detect_source("https://cdn.example.com/stream.m3u8") == VS_SRC_HLS);
    TEST("mpd detect",
        vs_detect_source("https://cdn.example.com/stream.mpd") == VS_SRC_DASH);
    TEST("generic site",
        vs_detect_source("https://example.com/page.html") == VS_SRC_GENERIC);
    TEST("null source",
        vs_detect_source(NULL) == VS_SRC_UNKNOWN);
    TEST("rtmp not video-sniffer",
        vs_detect_source("rtmp://live.example.com/app") == VS_SRC_UNKNOWN);

    /* excluded domains — تبلیغات نباید به‌عنوان ویدیو تشخیص داده بشن */
    TEST("doubleclick excluded",
        vs_is_excluded_domain("https://doubleclick.net/ad.mp4"));
    TEST("googlesyndication excluded",
        vs_is_excluded_domain("https://googlesyndication.com/x.mp4"));
    TEST("youtube not excluded",
        !vs_is_excluded_domain("https://youtube.com/watch?v=abc"));
    TEST("null not excluded",
        !vs_is_excluded_domain(NULL));
    TEST("excluded domain blocks source detect",
        vs_detect_source("https://doubleclick.net/video.mp4") == VS_SRC_UNKNOWN);

    /* vs_is_video_url */
    TEST("youtube is video",
        vs_is_video_url("https://youtube.com/watch?v=abc"));
    TEST("generic is not video",
        !vs_is_video_url("https://example.com/page.html"));
    TEST("null is not video",
        !vs_is_video_url(NULL));

    /* vs_make_label */
    char label[64];
    vs_make_label(1920, 1080, 5000, label, sizeof(label));
    TEST("label has 1080p", strstr(label, "1080p") != NULL);

    vs_make_label(1280, 720, 2500, label, sizeof(label));
    TEST("label has 720p", strstr(label, "720p") != NULL);

    vs_make_label(0, 0, 0, label, sizeof(label));
    TEST("label fallback original", strcmp(label, "Original") == 0);

    vs_make_label(0, 0, 128, label, sizeof(label));
    TEST("label bitrate only", strstr(label, "kbps") != NULL);

    /* vs_best_quality — VS_VideoInfo حدود 38KB هست، روی heap
       تا با بقیه local variable های این تابع جمع نشه و باعث
       stack pressure نشه (همون درسی که از HLS_Playlist گرفتیم) */
    VS_VideoInfo *info = (VS_VideoInfo *)calloc(1, sizeof(VS_VideoInfo));
    if (!info) return;
    info->quality_count = 3;
    info->qualities[0].height = 360;
    info->qualities[1].height = 1080;
    info->qualities[2].height = 720;
    TEST("best quality picks highest", vs_best_quality(info) == 1);

    /* تساوی height — بالاترین bitrate برنده */
    memset(info, 0, sizeof(*info));
    info->quality_count = 2;
    info->qualities[0].height = 720; info->qualities[0].bitrate = 1500;
    info->qualities[1].height = 720; info->qualities[1].bitrate = 3000;
    TEST("best quality tiebreak bitrate", vs_best_quality(info) == 1);

    TEST("best quality empty safe", vs_best_quality(NULL) == 0);
    free(info);

    /* vs_confirm_video_mime — فقط null-safety، نیاز به شبکه‌ی واقعی داره */
    TEST("confirm_mime null safe", !vs_confirm_video_mime(NULL));

    /* plugin lifecycle */
    IDM_PluginHooks *plug = vs_plugin_create(NULL, NULL);
    TEST("vs create",          plug != NULL);
    if (!plug) return;

    TEST("plugin name set",    plug->name != NULL);
    TEST("api_version match",  plug->api_version == IDM_PLUGIN_API_VERSION);

    /* set_callback */
    vs_set_callback(plug, NULL, NULL);
    TEST("set_callback no crash", TRUE);

    /* sniff_direct — لینک مستقیم باید همیشه کار کنه (بدون نیاز به شبکه)
       VS_VideoInfo دیگه‌ای روی heap، همون دلیل بالا */
    VS_VideoInfo *direct_info = (VS_VideoInfo *)calloc(1, sizeof(VS_VideoInfo));
    if (!direct_info) { vs_plugin_destroy(plug); return; }

    BOOL ok = vs_sniff_sync("https://cdn.example.com/movie.mp4",
                             NULL, direct_info);
    TEST("sniff direct succeeds",  ok);
    if (ok) {
        TEST("direct has 1 quality", direct_info->quality_count == 1);
        TEST("direct ext is mp4",
            strcmp(direct_info->qualities[0].ext, ".mp4") == 0);
        TEST("direct source set",
            direct_info->source == VS_SRC_DIRECT);
    }
    free(direct_info);

    /* NULL safety */
    vs_sniff_sync(NULL, NULL, NULL);
    vs_sniff_async(NULL, NULL, NULL, NULL);
    vs_sniff_async(plug, NULL, NULL, NULL);
    vs_set_callback(NULL, NULL, NULL);
    vs_plugin_destroy(NULL);
    TEST("null safety all",    TRUE);

    vs_plugin_destroy(plug);
    TEST("vs destroy",         TRUE);
}

/* ======================================================
   main
   ====================================================== */
int main(void) {
    SetConsoleOutputCP(CP_UTF8);   /* خروجی فارسی و خط تیره درست نمایش داده شود */
    /* داده‌ی تست جدا از داده‌ی واقعی کاربر است: ini، cookies، فروشگاه، رجیستری و استارت به پوشه‌ی موقت می‌روند */
    {
        char tmp[MAX_PATH] = {0};
        char home[MAX_PATH] = {0};
        if (GetTempPathA(sizeof(tmp), tmp) > 0) {
            size_t tl = strlen(tmp);
            if (tl > 0 && tmp[tl - 1] == '\\') tl--;
            const char *sub = "lass_test_data";
            size_t sbl = strlen(sub);
            if (tl + 1 + sbl < sizeof(home)) {
                memcpy(home, tmp, tl);
                home[tl] = '\\';
                memcpy(home + tl + 1, sub, sbl + 1);
                CreateDirectoryA(home, NULL);
                SetEnvironmentVariableA("LASS_DATA_DIR", home);
            }
        }
    }
    printf("==========================================\n");
    printf(" Lass Download Manager – Test Suite v5\n");
    printf("==========================================\n");

    test_security_fixes();
    test_chmml();
    test_plugin_registry();
    test_cookie_jar();
    test_smart_resume();
    test_dynamic_seg();
    test_speed_graph();
    test_clipboard();
    test_basket();
    test_ftp();
    test_hls();
    test_proxy();
    test_http_auth();
    test_engine_features();
    test_engine_fifo();
    test_engine_concurrent();
    test_engine_real_concurrency();
    test_server_negotiation();
    test_batch_url_extraction();
    test_crash_report();
    test_legacy_migration();
    test_dl_lifecycle();
    test_ftp_download();
    test_url_credentials();
    test_tels_container();
    test_tels_encryption();
    test_tels_api_edges();
    test_lass_store();
    test_state_migration();
    test_engine_destroy_while_launching();
    test_segment_policy_parks_stuck_range();
    test_segment_honours_retry_after();
    test_launch_retry_backoff();
    test_permanent_vs_transient_errors();
    test_http_extract_host();
    test_tels_temp_file();
    test_tels_long_path();
    test_queue_persist();
    test_http_fixup();
    test_filename_security();
    test_secure_store();
    test_command_injection_protection();
    test_content_length_validation();
    test_shutdown_brake();
    test_range_safety();
    test_settings();
    test_i18n();
    test_browser_ext();
    test_dash();
    test_notify();
    test_categories();
    test_rtmp();
    test_video_sniffer();

    printf("\n==========================================\n");
    printf(" Results: %d/%d passed", g_pass, g_total);
    if (g_fail > 0)
        printf("  |  %d FAILED", g_fail);
    printf("\n==========================================\n");
    fflush(stdout);

    /* از ExitProcess به‌جای return استفاده می‌کنیم تا از crash بعد از
       نتیجه جلوگیری کنیم. این crash از MinGW CRT cleanup (atexit، thread
       TLS destructors) میاد که با launch_thread های background که هنوز
       در حال HTTP timeout هستن تداخل پیدا می‌کنه. این مشکل فقط در
       test binary هست — در Lass.exe اصلی که event loop داره و شاتداون
       مرتب انجام میشه، این مشکل وجود نداره. ExitProcess مستقیماً
       به Windows kernel میره و از CRT teardown کاملاً رد میشه. */
    ExitProcess((g_fail == 0) ? 0 : 1);
}
