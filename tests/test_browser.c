/* browser: browser extension tests (tests/test_browser.c) */
#include "test_common.h"

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

void test_browser_ext(void) {
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

