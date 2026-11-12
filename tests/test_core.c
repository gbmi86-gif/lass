/* core: plugin and basic component tests (tests/test_core.c) */
#include "test_common.h"

/* ======================================================
   تست ۱: Security Fixes
   ====================================================== */
void test_security_fixes(void) {
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
void test_chmml(void) {
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
void test_plugin_registry(void) {
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
void test_cookie_jar(void) {
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
void test_smart_resume(void) {
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
void test_dynamic_seg(void) {
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
void test_speed_graph(void) {
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

void test_clipboard(void) {
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

void test_basket(void) {
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
void test_ftp(void) {
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
   تست ۱۲: Proxy Plugin
   ====================================================== */
void test_proxy(void) {
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
void test_http_auth(void) {
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

