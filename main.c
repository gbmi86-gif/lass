/* main.c
   ─────────────────────────────────────────────────────────────
   Lass Download Manager – Entry Point

   فقط سه کار:
     1. بارگذاری تنظیمات
     2. ساخت Core (engine + plugins)
     3. ساخت GUO (UI) و اجرای message loop
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>

#include "../src/settings.h"
#include "../core/core_ldm.h"
#include "../core/guo.h"
#include "../src/lass_log.h"
#include "../src/shutdown_brake.h"
#include "../src/version.h"
#include "../src/http.h"
#include "../src/crash_report.h"
#include "../src/i18n.h"
#include "../plugins/plugin_browser_ext.h"

/* ── Global Exception Handler ──
   اگه برنامه crash کنه (access violation و غیره)، قبل از مردن
   اطلاعات رو در لاگ ثبت می‌کنه — این به ما کمک می‌کنه دفعه‌ی بعد
   دقیقاً بفهمیم crash از کجا اومده، به‌جای اینکه فقط ببینیم
   "برنامه بسته شد" بدون هیچ سرنخی. */
static LONG WINAPI lass_crash_handler(EXCEPTION_POINTERS *ep) {
    /* اگه خودِ handler (یا لاگ‌گیری داخلش) exception بده، دوباره
       وارد همین تابع نشیم و در حلقه نیفتیم. */
    static volatile LONG in_handler = 0;
    if (InterlockedExchange(&in_handler, 1)) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;

    LOGE(LOG_MOD_CORE,
         "*** UNHANDLED EXCEPTION *** code=0x%08lX addr=%p",
         (unsigned long)code, addr);

    const char *desc = "Unknown";
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:    desc = "Access Violation (use-after-free / null deref)"; break;
    case EXCEPTION_STACK_OVERFLOW:      desc = "Stack Overflow"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  desc = "Divide by Zero"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: desc = "Array Bounds Exceeded"; break;
    }
    LOGE(LOG_MOD_CORE, "Exception type: %s", desc);

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR access_type = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR target_addr = ep->ExceptionRecord->ExceptionInformation[1];
        LOGE(LOG_MOD_CORE, "Access violation: %s at address 0x%p",
             access_type == 0 ? "READ" : (access_type == 1 ? "WRITE" : "EXECUTE"),
             (void*)target_addr);
    }

    /* محل دقیق خطا (ماژول+offset) و call stack — بدون این، فقط یه
       آدرس خام ASLR‌شده می‌موند که به‌تنهایی قابل استفاده نیست. */
    crash_report_log(ep);

    log_flush();
    log_destroy();

    /* اجازه بده Windows دیالوگ معمول crash رو نشون بده (یا دیباگر
       جلوش رو بگیره) — ما فقط می‌خوایم قبلش لاگ ثبت بشه */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── Native Messaging Host Mode ──
   وقتی Chrome/Firefox می‌خوان با Lass صحبت کنن، Lass.exe رو
   با آرگومان --native-host اجرا می‌کنن. این حالت stdin رو
   می‌خونه (فرمت native messaging: 4 بایت length-prefix + JSON)،
   و پیام رو از طریق named pipe به instance اصلی Lass می‌فرسته،
   جواب رو برمی‌گردونه، و خارج میشه.
   این یعنی یه Lass.exe جداگانه (به‌عنوان native host) اجرا میشه
   ولی فقط پل بین مرورگر و instance اصلی Lassه. */
static int run_as_native_host(void) {
    /* ۱. خوندن پیام از stdin با فرمت native messaging
       (4 بایت little-endian length + JSON body) */
    HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE)
        return 1;

    uint32_t msg_len = 0;
    DWORD    nread   = 0;

    if (!ReadFile(hin, &msg_len, 4, &nread, NULL) || nread != 4)
        return 1;
    if (msg_len == 0 || msg_len > 1024 * 1024) return 1; /* sanity */

    char *msg = (char *)malloc(msg_len + 1);
    if (!msg) return 1;
    if (!ReadFile(hin, msg, msg_len, &nread, NULL) || nread != msg_len) {
        free(msg); return 1;
    }
    msg[msg_len] = '\0';

    /* ۲. فرستادن به named pipe که server_thread اصلی Lass گوش میده
       (LASS_PIPE_NAME از plugin_browser_ext.h میاد — تنها منبع،
       تا اگه یه‌روز عوض بشه، همه‌جا خودکار sync بمونه) */
    HANDLE pipe = CreateFileA(
        LASS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        /* Lass باز نیست — یه جواب خطا به مرورگر برگردون */
        const char *err = "{\"error\":\"Lass is not running\"}";
        uint32_t elen = (uint32_t)strlen(err);
        DWORD w;
        WriteFile(hout, &elen, 4, &w, NULL);
        WriteFile(hout, err, elen, &w, NULL);
        free(msg); return 0;
    }

    /* باگ واقعی که این‌جا رفع شد: سمت دیگه‌ی این pipe (client_thread
       در plugin_browser_ext.c) دقیقاً همون فریم native messaging رو
       انتظار داره: ۴ بایت طول + payload. این‌جا قبلاً فقط payload
       (بدون طول) نوشته می‌شد — یعنی client_thread چهار بایت اول
       خودِ متن JSON (مثلاً بایت‌های `{"ac`) رو به‌جای طول واقعی
       می‌خوند، یه عدد بی‌معنی/خیلی بزرگ درمیومد، از چک sanity رد
       می‌شد و بی‌سروصدا connection رو می‌بست — یعنی on_download()
       هیچ‌وقت واقعاً صدا زده نمی‌شد و دانلود از مرورگر به Lass
       نمی‌رسید، هرچند popup همیشه "Connected" نشون می‌داد چون جواب
       ثابت زیر همیشه بدون توجه به این مسیر برگردونده می‌شد. */
    DWORD w;
    WriteFile(pipe, &msg_len, 4, &w, NULL);
    WriteFile(pipe, msg, msg_len, &w, NULL);
    free(msg);

    /* ۳. جواب واقعی رو از client_thread بخون و همون رو (نه یه پیام
       ثابت جعلی) به مرورگر برگردون — همون فریم‌بندی طول+payload،
       چون client_thread هم دقیقاً همینطور جواب می‌نویسه. اگه هر
       جای این مرحله شکست خورد (مثلاً Lass وسط کار بسته شد)، یه
       خطای واضح برمی‌گردونیم به‌جای وانمود کردن به موفقیت. */
    uint32_t resp_len = 0;
    DWORD r = 0;
    BOOL got_len = ReadFile(pipe, &resp_len, 4, &r, NULL) && r == 4
                   && resp_len > 0 && resp_len < (1024 * 1024);

    char *resp = got_len ? (char *)malloc(resp_len + 1) : NULL;
    BOOL got_body = FALSE;
    if (resp) {
        DWORD total = 0;
        got_body = TRUE;
        while (total < resp_len) {
            DWORD rr = 0;
            if (!ReadFile(pipe, resp + total, resp_len - total, &rr, NULL) || rr == 0) {
                got_body = FALSE;
                break;
            }
            total += rr;
        }
        if (got_body) resp[resp_len] = '\0';
    }
    CloseHandle(pipe);

    if (got_len && got_body) {
        WriteFile(hout, &resp_len, 4, &w, NULL);
        WriteFile(hout, resp, resp_len, &w, NULL);
        free(resp);
    } else {
        free(resp);
        const char *err = "{\"error\":\"no response from Lass\"}";
        uint32_t elen = (uint32_t)strlen(err);
        WriteFile(hout, &elen, 4, &w, NULL);
        WriteFile(hout, err, elen, &w, NULL);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)nShow;

    /* ── Native Host Mode ──
       مرورگر این exe رو با آرگومان --native-host اجرا می‌کنه.
       باید قبل از هر چیز (حتی قبل از single-instance check) چک
       بشه تا هر بار که Chrome/Firefox پیام می‌فرسته، یه instance
       موقت سریع وارد و خارج میشه بدون اینکه UI اصلی تداخل داشته
       باشه. */
    if (lpCmd && strstr(lpCmd, "--native-host")) {
        return run_as_native_host();
    }

    /* ── Unregister mode ──
       برای uninstaller (Inno Setup) — قبل از حذف خودِ exe صدا زده
       میشه تا ورودی‌های native messaging مرورگرها (که به این exe
       اشاره می‌کنن) پاک بشن، وگرنه بعد از uninstall یه registry
       entry باقی می‌مونه که به فایلی اشاره می‌کنه که دیگه وجود
       نداره — دقیقاً همون کلاس مشکلی که برای lass_host.json حل
       شد، این‌بار برای خودِ exe. بدون UI، بی‌صدا خارج می‌شه. */
    if (lpCmd && strstr(lpCmd, "--unregister-native-host")) {
        browser_ext_unregister(BROWSER_CHROME | BROWSER_FIREFOX | BROWSER_EDGE);
        return 0;
    }

    SetUnhandledExceptionFilter(lass_crash_handler);

    /* ── single instance ── */
    HANDLE mutex = CreateMutexA(NULL, TRUE, "LassDownloadManager_v5");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowA("LassMainWnd", NULL);
        if (existing) { ShowWindow(existing, SW_SHOW); SetForegroundWindow(existing); }
        CloseHandle(mutex);
        return 0;
    }

    /* ── init COM / CommonControls ── */
    OleInitialize(NULL);
    InitCommonControls();

    /* ── رفع سقف WinINet برای connection همزمان به یه host ──
       باید قبل از هر InternetOpenA (یعنی قبل از engine_create و
       هر بار discovery/download) صدا زده بشه، وگرنه بی‌اثره.
       جزئیات کامل چرا این لازمه: بالای http_init در src/http.c */
    http_init();

    /* ── تنظیمات ── */
    LassSettings cfg;
    settings_load(&cfg);

    /* ── زبان UI ──
       باید قبل از ساخت هر پنجره‌ای صدا زده بشه، چون create_menubar/
       create_listview/create_toolbar متن‌هاشون رو یه‌بار موقع ساخت
       از i18n_str می‌خونن — تغییر زبان بعد از ساخت پنجره‌ها روی
       متن‌های از‌قبل-نوشته‌شده اثر نمی‌ذاره (نیاز به راه‌اندازی
       مجدد پنجره‌ها داره، که فعلاً پیاده‌سازی نشده — محدودیت
       شناخته‌شده‌ی این فاز اول i18n). */
    i18n_set_language(i18n_lang_from_code(cfg.language));

    /* ── logger ── */
    {
        char log_path[MAX_PATH];
        settings_get_ini_path(log_path, sizeof(log_path));
        char *dot = strrchr(log_path, '.');
        if (dot) strcpy(dot, ".log");
        log_init(log_path, LOG_INFO);
    }
    LOGI(LOG_MOD_CORE, "Lass Download Manager v" LASS_VERSION_STRING " starting");

    /* ── باگ واقعی که اینجا فیکس شد: «تیک Start with Windows فعاله
       ولی بعد از ری‌استارت سیستم اجرا نمی‌شه» ──
       settings_set_startup (نوشتن HKCU\...\Run) فقط یه‌بار، دقیقاً
       همون لحظه‌ای که کاربر تیک رو می‌زنه، صدا زده می‌شد (در
       guo.c). بعدش دیگه هیچ‌وقت دوباره چک/بازنویسی نمی‌شد — و
       cfg.run_on_startup (که تیک‌خورده بودنِ چک‌باکس رو کنترل
       می‌کنه) فقط یه «نیت» ذخیره‌شده توی lass.ini هست، نه وضعیت
       واقعی registry. یعنی اگه مقدار registry به هر دلیلی از بین
       بره یا قدیمی بمونه — جابه‌جایی/آپدیت exe (GetModuleFileNameA
       مسیر رو موقع نوشتن ثابت می‌کنه)، پاک‌سازی توسط آنتی‌ویروس/
       ابزار cleanup، یا نصب نسخه‌ی جدید در مسیر متفاوت — چک‌باکس
       همچنان «فعال» نشون داده می‌شه ولی عملاً هیچ‌چیزی اجرا نمی‌شه،
       و settings_get_startup (تابعی که واقعاً registry رو می‌خونه)
       قبلاً هیچ‌جای برنامه‌ی واقعی صدا زده نمی‌شد تا این افتراق رو
       تشخیص بده.

       فیکس: دقیقاً مثل browser_ext_register زیر همین کامنت، هر بار
       که برنامه واقعاً اجرا می‌شه (چه با دابل‌کلیک چه از Startup)،
       اگه نیت کاربر "روشن" بوده، registry entry رو با مسیر *فعلی*
       exe دوباره می‌نویسیم — idempotent و بی‌صدا، همون الگوی
       self-heal که از قبل برای browser extension وجود داشت. */
    if (cfg.run_on_startup) {
        BOOL healed = settings_set_startup(TRUE);
        if (!healed)
            LOGW(LOG_MOD_CORE,
                 "could not refresh startup registry entry (HKCU\\...\\Run) — "
                 "\"Start with Windows\" may not work until re-toggled in Settings");
    }

    /* ── ثبت خودکار native messaging host برای مرورگرها ──
       نکته‌ی مهمی که Juliya درست گفت: بیشتر دانلود منیجرها اصلاً
       از کاربر نمی‌پرسن «می‌خوای ثبت بشه؟» — خودشون در پس‌زمینه
       ثبتش می‌کنن، و این خودِ مرورگره که (اگه لازم باشه) از کاربر
       می‌پرسه «می‌خوای این extension فعال بشه؟». قبلاً این‌جا فقط
       با زدن دستیِ دکمه‌ی Settings→Browser Extension→Register All
       اتفاق می‌افتاد — که هم با رفتار رایج صنعت فرق داشت، هم دقیقاً
       همون چیزی بود که باعث شد registry اول ثبت نشده باشه و خطای
       "native messaging host not found" بگیریم.

       الان بی‌صدا و idempotent هر بار استارت صدا زده می‌شه (چند
       تا RegSetValueExA + نوشتن یه فایل کوچیک — هزینه‌اش ناچیزه)،
       بدون هیچ UI/پیام. برای مرورگری که اصلا نصب نباشه، این فقط
       یه registry entry بی‌اثر و بدون ضرر می‌سازه که هیچ‌وقت
       استفاده نمی‌شه. این باعث میشه اگه بعداً exe جابه‌جا بشه یا
       registry به هر دلیلی خراب/ناقص بشه، خودش رو هر بار startup
       خودکار ترمیم کنه، بدون نیاز به یادآوری دستی کاربر.

       دکمه‌ی Register All توی Settings هم دست‌نخورده می‌مونه — برای
       ثبت فوری/دستی (مثلاً بلافاصله بعد از یه تغییر، بدون نیاز به
       ری‌استارت کامل برنامه) هنوز مفیده، ولی دیگه شرط لازم نیست. */
    browser_ext_register(BROWSER_ALL);

    /* /minimized argument */
    if (lpCmd && strstr(lpCmd, "/minimized"))
        cfg.start_minimized = TRUE;

    /* ── core ── */
    LDM_Core *core = core_create(NULL);
    if (!core) {
        MessageBoxA(NULL, "Failed to initialize download engine.",
                    "Lass", MB_ICONERROR);
        OleUninitialize();
        ReleaseMutex(mutex); CloseHandle(mutex);
        return 1;
    }

    /* اعمال تنظیمات startup به core */
    core_settings_apply(core, &cfg);

    /* ── GUO ── */
    GUO_Context guo;
    if (!guo_init(&guo, hInst, core, &cfg)) {
        MessageBoxA(NULL, "Failed to create main window.",
                    "Lass", MB_ICONERROR);
        core_destroy(core);
        OleUninitialize();
        ReleaseMutex(mutex); CloseHandle(mutex);
        return 1;
    }

    /* ── message loop ── */
    int exit_code = guo_run(&guo);

    /* ── ترمز خروج: قبل از هر cleanup دیگه، مطمئن شو همه‌ی
       subsystem های ثبت‌شده (scheduler اصلی engine، دانلودهای
       در حال launch، speed_scheduler و غیره) واقعاً تموم شدن.
       اولین چیزی که register شده (engine_scheduler) آخرین
       چیزیه که منتظرش می‌مونیم — چون همه‌چیز دیگه بهش وابسته‌ست.
       این دقیقاً همون "ترمز خروج ناگهانی" هست که قرار بود از
       همه دیرتر خارج بشه.

       ── باگ واقعی که در لاگ پیدا شد و اینجا فیکس شد ──
       قبلاً shutdown_brake_wait_all درست همین‌جا، *قبل* از
       guo_destroy/core_destroy صدا زده می‌شد. مشکل: سیگنال واقعیِ
       «باید متوقف بشی» به engine_scheduler و ۴ تا thread پلاگین
       (dynamic_seg_monitor، speed_graph_timer، speed_scheduler،
       browser_ext_server) فقط *داخل* core_destroy اتفاق می‌افته.
       یعنی wait_all همیشه، قطعاً، برای thread‌هایی صبر می‌کرد که
       هنوز اصلاً خبردار نشده بودن باید خارج بشن — یه timeout
       تضمین‌شده‌ی ۵ ثانیه‌ای روی هر session، دقیقاً عین چیزی که در
       lass.log واقعی دیده شد (هر بار، عیناً همین ۵ subsystem).
       بدتر: core_destroy قبلاً plugins_shutdown(NULL) صدا می‌زد که
       به‌خاطر گاردش، اون ۴ پلاگین رو هیچ‌وقت واقعاً destroy
       نمی‌کرد — یعنی thread‌هاشون حتی *بعد* از این timeout هم
       دست‌نخورده به کارشون ادامه می‌دادن، هم‌زمان با اینکه
       core_destroy پایین‌تر داشت حافظه‌ی engine/segments رو آزاد
       می‌کرد. این ترکیب دقیقاً با use-after-free نهایی توی لاگ
       (Access violation: WRITE در آدرس ۰x24، بعد از لاگ «shutdown
       complete») هم‌خونی داره.

       فیکس: الان core_signal_shutdown فقط پرچم‌ها رو ست می‌کنه
       (بدون wait)، *قبل* از wait_all صدا زده میشه — پس تا وقتی
       wait_all شروع به صبر کردن می‌کنه، همه‌ی ۵ subsystem از قبل
       خبردار شدن و واقعاً دارن خارج می‌شن. guo_destroy/core_destroy
       طبق معمول بعداً صدا زده می‌شن و کار join واقعی + آزادسازی رو
       انجام می‌دن — که حالا چون thread‌ها از قبل چند صد میلی‌ثانیه
       وقت داشتن، تقریباً همیشه فوری/بدون timeout تموم می‌شه. */
    core_signal_shutdown(core);
    shutdown_brake_begin_drain();
    if (!shutdown_brake_wait_all(5000)) {
        LOGW(LOG_MOD_CORE,
             "Some subsystems did not shut down cleanly within timeout — "
             "proceeding with forced cleanup anyway");
    }

    /* ── cleanup ── */
    guo_destroy(&guo);
    core_destroy(core);
    settings_save(&cfg);
    LOGI(LOG_MOD_CORE, "Lass shutdown complete");
    log_destroy();
    OleUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exit_code;
}
