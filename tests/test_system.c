/* system: settings, i18n, crash, shutdown and misc tests (tests/test_system.c) */
#include "test_common.h"

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

void test_crash_report(void) {
    SECTION("Crash Report (fault location + call stack)");

    int frames = crash_depth_probe(3);
    TEST("synthetic context walks multiple frames", frames >= 3);
    TEST("walk is bounded", frames <= 24);
    TEST("NULL pointers are safe", crash_report_log(NULL) == 0);

    EXCEPTION_POINTERS empty = { NULL, NULL };
    TEST("missing exception record is safe", crash_report_log(&empty) == 0);
}

/* ======================================================
   تست: accept_ranges / got_partial logic
   ====================================================== */
/* ======================================================
   تست: امنیت — Path Traversal در filename extraction
   ====================================================== */
void test_filename_security(void) {
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
   تست: Shutdown Brake — ترمز خروج ناگهانی
   ====================================================== */
void test_shutdown_brake(void) {
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

void test_settings(void) {
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
void test_i18n(void) {
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
   تست ۱۹: Notification Plugin
   ====================================================== */
void test_notify(void) {
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
void test_categories(void) {
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
void test_rtmp(void) {
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
void test_video_sniffer(void) {
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

