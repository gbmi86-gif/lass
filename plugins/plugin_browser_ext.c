/* plugin_browser_ext.c
   ─────────────────────────────────────────────────────────────
   Browser Extension Integration  –  Named Pipe + Native Messaging

   جریان:
     1. plugin یه Named Pipe server باز می‌کنه
     2. native_host.exe (helper کوچک) از stdin می‌خونه،
        به pipe می‌نویسه — extension با آن صحبت می‌کنه
     3. هر client connection در یه thread جداگانه serve میشه
     4. JSON ساده parse می‌کنیم بدون lib خارجی

   Native Messaging Protocol:
     Chrome/Firefox از stdout/stdin با frame:
       [4 bytes little-endian length][JSON payload]
     ما این رو از طریق named pipe emulate می‌کنیم
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>   /* PathCombineA */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../src/url_secure.h"
#include "plugin_browser_ext.h"
#include "../src/shutdown_brake.h"
#include "../src/lass_log.h"
#include "../src/version.h"
#include "../src/settings.h"

#define EXT_MAX_CLIENTS  8
#define EXT_BUF_SIZE     (64 * 1024)
#define EXT_VERSION      LASS_VERSION_STRING

/* توجه: قبلاً اینجا یه قفل خصوصی (g_log_lock) بود که فقط جلوی
   تداخل چندتا ext_log با هم رو می‌گرفت. الان از قفل سراسری
   log_console_lock/unlock (در lass_log.c) استفاده می‌کنیم که
   با همه‌ی منابع خروجی کنسول در کل process هماهنگه — نه فقط
   با خود ext_log. */

static void ext_log(const char *fmt, ...) {
    /* قبلاً این تابع فقط با vfprintf(stderr,...) می‌نوشت — یعنی توی
       Lass.exe واقعی (بدون کنسول) هیچ‌وقت جایی دیده نمی‌شد، حتی
       با وجود قفل مشترک console lock. الان از طریق log_write واقعی
       می‌ره که هم به فایل lass.log می‌نویسه هم echo به stderr می‌ده
       (خودش قفل داخلی مخصوص به خودشو داره، نیازی به
       log_console_lock دستی این‌جا نیست). */
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    /* پیام‌های ورودی این تابع قبلاً خودشون یه "\n" انتهایی داشتن
       (برای fprintf) — log_write خودش یه "\n" اضافه می‌کنه، پس
       trailing newline رو اینجا حذف می‌کنیم تا خط خالی اضافه
       توی لاگ نیفته. */
    size_t n = strlen(body);
    if (n > 0 && body[n-1] == '\n') body[n-1] = '\0';
    LOGI(LOG_MOD_EXTBR, "%s", body);
}

/* ── JSON helper: extract string value for key ── */
static BOOL json_get_str(const char *json, const char *key,
                          char *out, int out_sz) {
    if (!json || !key || !out) return FALSE;
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return FALSE;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return FALSE;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\') { p++; if (!*p) break; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0);
}

/* ── ساخت JSON response ── */
static void json_ok(char *out, int sz, uint32_t id) {
    snprintf(out, sz, "{\"status\":\"ok\",\"id\":%u,\"version\":\"%s\"}",
             id, EXT_VERSION);
}
static void json_pong(char *out, int sz) {
    snprintf(out, sz,
             "{\"status\":\"pong\",\"version\":\"%s\",\"name\":\"%s\"}",
             EXT_VERSION, LASS_EXT_HOST_NAME);
}
static void json_err(char *out, int sz, const char *msg) {
    snprintf(out, sz, "{\"status\":\"error\",\"message\":\"%s\"}", msg);
}

/* ======================================================
   ساختار داخلی
   ====================================================== */
typedef struct {
    HANDLE           pipe;
    struct BExt_Priv *priv;
} ClientCtx;

typedef struct BExt_Priv {
    HANDLE           server_thread;
    volatile BOOL    running;
    ExtDownloadCb    on_download;
    void            *userdata;
    volatile int     client_count;
    /* تعداد پیام‌هایی که واقعاً کامل خونده و parse شدن — صرف‌نظر
       از نوع action یا موفقیت dispatch. برای تشخیص «پیام اصلاً به
       سرور نرسید» از «پیام رسید ولی on_download صدا زده نشد»،
       مستقل از لاگ (که ممکنه زیر ctest به‌هم‌ریخته/buffered بشه). */
    volatile LONG    messages_received;
    CRITICAL_SECTION lock;
    /* handle های client thread ها برای WaitForMultipleObjects در
       destroy — بدون این، client_count==0 فقط یعنی thread داره
       return 0 می‌کنه، نه اینکه OS کاملاً exit کرده */
    HANDLE           client_handles[EXT_MAX_CLIENTS];
    volatile int     client_handle_count;
    CRITICAL_SECTION handles_lock;
} BExt_Priv;

/* ======================================================
   Client handler thread
   ====================================================== */
static DWORD WINAPI client_thread(LPVOID param) {
    ClientCtx *ctx  = (ClientCtx *)param;
    BExt_Priv *priv = ctx->priv;
    HANDLE     pipe = ctx->pipe;
    free(ctx);

    int brake_id = shutdown_brake_register("browser_ext_client");

    /* توجه: client_count از قبل در server_thread، قبل از
       CreateThread، افزایش پیدا کرده — اینجا فقط decrement
       می‌کنیم (در done:) تا گپ زمانی race-condition بسته بمونه */

    char *buf = (char *)malloc(EXT_BUF_SIZE);
    if (!buf) { CloseHandle(pipe); goto done; }

    char *resp = (char *)malloc(EXT_BUF_SIZE);
    if (!resp) { free(buf); CloseHandle(pipe); goto done; }

    while (priv->running) {
        /* Native Messaging frame: 4 bytes length + payload */
        DWORD bytes_read = 0;
        uint32_t msg_len = 0;

        /* خواندن length header —
           ── باگ واقعی که اینجا فیکس شد ──
           قبلاً این فقط یه ReadFile تنها بود: `if (!ok ||
           bytes_read != 4) break;`. مشکل: ReadFile روی named pipe
           می‌تونه کمتر از مقدار درخواستی برگردونه (partial read) —
           این یه رفتار مستند و قانونیه، نه یه خطا. خوندن payload
           درست زیر همین (چند خط پایین‌تر) این رو با یه حلقه‌ی
           واقعی (`while (total < msg_len)`) درست مدیریت می‌کنه، ولی
           خوندن ۴ بایت length header هیچ‌وقت این حلقه رو نداشت —
           یه partial read همینجا کل connection رو silently می‌بست.
           این دقیقاً توضیح می‌ده چرا پیام اول (ping) روی یه
           connection تازه تقریباً همیشه جواب می‌ده ولی پیام دوم
           (download، بلافاصله بعد از نوشتن response پیام اول) گاهی
           بی‌صدا گم می‌شه: دقیقاً شرایطی که باعث partial read
           می‌شه (OS هنوز داره بخش اول بافر رو delivery می‌ده) برای
           پیام دوم روی یه connection که تازه مشغول بوده محتمل‌تره. */
        BOOL ok = TRUE;
        bytes_read = 0;
        while (bytes_read < 4) {
            DWORD r = 0;
            ok = ReadFile(pipe, ((BYTE *)&msg_len) + bytes_read, 4 - bytes_read, &r, NULL);
            if (!ok || r == 0) break;
            bytes_read += r;
        }
        if (!ok || bytes_read != 4) break;
        if (msg_len == 0 || msg_len >= (uint32_t)EXT_BUF_SIZE) break;

        /* خواندن JSON payload */
        DWORD total = 0;
        while (total < msg_len) {
            DWORD r = 0;
            ok = ReadFile(pipe, buf + total, msg_len - total, &r, NULL);
            if (!ok || r == 0) goto cleanup;
            total += r;
        }
        buf[msg_len] = '\0';

        char red_buf[2048];
        url_redact_for_log(buf, red_buf, sizeof(red_buf));
        ext_log("[BrowserExt] received: %.200s\n", red_buf);
        InterlockedIncrement(&priv->messages_received);

        /* parse action */
        char action[64] = {0};
        json_get_str(buf, "action", action, sizeof(action));

        uint32_t resp_id = 0;

        if (strcmp(action, "ping") == 0) {
            json_pong(resp, EXT_BUF_SIZE);

        } else if (strcmp(action, "download") == 0) {
            char url[2048]  = {0};
            char fname[256] = {0};
            char ref[2048]  = {0};

            json_get_str(buf, "url",      url,   sizeof(url));
            json_get_str(buf, "filename", fname, sizeof(fname));
            json_get_str(buf, "referer",  ref,   sizeof(ref));

            ext_log("[BrowserExt] dispatch: url='%s' on_download=%p\n",
                    url, (void *)priv->on_download);
            if (url[0] && priv->on_download) {
                priv->on_download(url, fname, ref, priv->userdata);
                ext_log("[BrowserExt] dispatch: on_download returned\n");
                resp_id = (uint32_t)GetTickCount(); /* placeholder ID */
                json_ok(resp, EXT_BUF_SIZE, resp_id);
            } else {
                json_err(resp, EXT_BUF_SIZE, "missing url");
            }

        } else {
            json_err(resp, EXT_BUF_SIZE, "unknown action");
        }

        /* ارسال response با length header */
        uint32_t resp_len = (uint32_t)strlen(resp);
        DWORD written = 0;
        WriteFile(pipe, &resp_len, 4, &written, NULL);
        WriteFile(pipe, resp, resp_len, &written, NULL);
        FlushFileBuffers(pipe);
    }

cleanup:
    free(buf);
    free(resp);
    CloseHandle(pipe);

done:
    InterlockedDecrement((LONG *)&priv->client_count);
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ======================================================
   Pipe server thread  –  accept loop
   ====================================================== */
static DWORD WINAPI server_thread(LPVOID param) {
    BExt_Priv *priv = (BExt_Priv *)param;
    int brake_id = shutdown_brake_register("browser_ext_server");

    ext_log("[BrowserExt] pipe server started: %s\n", LASS_PIPE_NAME);

    while (priv->running) {
        HANDLE pipe = CreateNamedPipeA(
            LASS_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            EXT_MAX_CLIENTS,
            EXT_BUF_SIZE, EXT_BUF_SIZE,
            5000,   /* timeout ms */
            NULL);

        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        /* منتظر client */
        BOOL connected = ConnectNamedPipe(pipe, NULL)
                         || (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!priv->running) {
            CloseHandle(pipe);
            break;
        }

        if (connected) {
            ClientCtx *ctx = (ClientCtx *)malloc(sizeof(*ctx));
            if (ctx) {
                ctx->pipe = pipe;
                ctx->priv = priv;
                InterlockedIncrement((LONG *)&priv->client_count);
                HANDLE t = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
                if (t) {
                    /* handle رو نگه می‌داریم تا destroy بتونه با
                       WaitForSingleObject منتظر exit واقعی thread
                       بمونه — نه فقط client_count==0 که قبل از
                       return 0 اتفاق می‌افته و هنوز thread روی
                       stack خودشه */
                    EnterCriticalSection(&priv->handles_lock);
                    int idx = priv->client_handle_count;
                    if (idx < EXT_MAX_CLIENTS) {
                        priv->client_handles[idx] = t;
                        priv->client_handle_count++;
                    } else {
                        CloseHandle(t); /* overflow — نباید اتفاق بیفته */
                    }
                    LeaveCriticalSection(&priv->handles_lock);
                } else {
                    InterlockedDecrement((LONG *)&priv->client_count);
                    free(ctx);
                    CloseHandle(pipe);
                }
            } else {
                CloseHandle(pipe);
            }
        } else {
            CloseHandle(pipe);
        }
    }

    ext_log("[BrowserExt] pipe server stopped\n");
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ======================================================
   Registry — ثبت Native Messaging Host
   ====================================================== */
static BOOL reg_set_host(const char *base_key, const char *manifest_path) {
    char full_key[512];
    snprintf(full_key, sizeof(full_key),
             "%s\\%s", base_key, LASS_EXT_HOST_NAME);

    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, full_key, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                         NULL, &hKey, NULL) != ERROR_SUCCESS)
        return FALSE;

    BOOL ok = (RegSetValueExA(hKey, NULL, 0, REG_SZ,
                               (const BYTE *)manifest_path,
                               (DWORD)(strlen(manifest_path) + 1))
               == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return ok;
}

static BOOL reg_delete_host(const char *base_key) {
    char full_key[512];
    snprintf(full_key, sizeof(full_key),
             "%s\\%s", base_key, LASS_EXT_HOST_NAME);
    LONG r = RegDeleteKeyA(HKEY_CURRENT_USER, full_key);
    return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
}

static BOOL write_manifest(char *out_path, int out_sz) {
    /* مسیر Lass.exe خودمون — این همون exe هست که با --native-host
       اجرا میشه تا native messaging رو handle کنه. نیازی به helper
       جداگانه نیست. */
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));

    /* قبلاً manifest (lass_host.json) کنار خودِ exe نوشته می‌شد —
       یعنی اگه کاربر exe رو (مثلاً روی Desktop، برای تست) پاک کنه
       یا جابه‌جا/rebuild کنه، این فایل هم با اون از بین می‌رفت و
       registry به یه فایل گم‌شده اشاره می‌کرد (هماهنگی مرورگر از
       کار می‌افتاد) — دقیقاً همون مشکلی که گزارش شد.

       الان به‌جاش توی همون پوشه‌ای می‌نویسیم که lass.ini توش
       ذخیره می‌شه (%APPDATA%\Lass\) — یه آدرس ثابت که مستقل از
       این‌که exe کجا اجرا بشه یا چندبار recompile/کپی بشه، همیشه
       همون‌جا می‌مونه. registry هم به همین آدرس ثابت اشاره می‌کنه
       (در reg_set_host پایین‌تر)، پس دیگه با پاک کردن یا جابه‌جایی
       exe از بین نمی‌ره. */
    char dir[MAX_PATH * 2];
    {
        char ini_path[MAX_PATH * 2];
        settings_get_ini_path(ini_path, sizeof(ini_path));
        char *bs = strrchr(ini_path, '\\');
        if (bs) {
            int dlen = (int)(bs - ini_path);
            if (dlen >= (int)sizeof(dir)) dlen = (int)sizeof(dir) - 1;
            memcpy(dir, ini_path, dlen);
            dir[dlen] = '\0';
        } else {
            /* بعید — settings_get_ini_path همیشه یه مسیر کامل
               برمی‌گردونه — ولی برای اطمینان یه fallback امن. */
            GetTempPathA(sizeof(dir), dir);
        }
    }

    if ((int)strlen(dir) + 22 > out_sz)
        GetTempPathA(sizeof(dir), dir);

    PathCombineA(out_path, dir, "lass_host.json");

    /* ساخت command line: "C:\path\Lass.exe --native-host"
       Chrome این رو اجرا می‌کنه و stdin/stdout رو handle می‌کنه */
    char cmd[MAX_PATH * 2] = {0};
    snprintf(cmd, sizeof(cmd), "%s --native-host", exe);

    /* escape backslashes برای JSON */
    char escaped[MAX_PATH * 2 + 32] = {0};
    for (int i = 0, j = 0; cmd[i] && j < (int)sizeof(escaped)-2; i++) {
        if (cmd[i] == '\\') escaped[j++] = '\\';
        escaped[j++] = cmd[i];
    }

    /* Chrome و Firefox هر کدوم یه manifest جداگانه دارن:
       - Chrome: allowed_origins با extension ID (یا empty برای
         unpacked که رجیستری key کافیه)
       - Firefox: allowed_extensions با gecko ID
       
       نکته: Chrome MV3 دیگه wildcard "*" رو در allowed_origins
       قبول نمی‌کنه (از Chrome 91+). یه راه‌حل اینه که manifest
       رو بدون allowed_origins بنویسیم — Chrome برای extensions
       که از مسیر رجیستری load میشن، registry key خودش رو به‌عنوان
       permission می‌شناسه و allowed_origins اختیاریه. */
    /* Chrome extension ID محاسبه‌شده از کلید عمومی ثابت که در
       extensions/lass-chrome/manifest.json (فیلد "key") قرار
       داره. این ID همیشه یکسانه چون از یه کلید ثابت میاد — نه
       تصادفی بر اساس مسیر نصب. این یعنی native messaging بدون
       نیاز به انتشار در Chrome Web Store کار می‌کنه.
       (محاسبه: SHA256 روی DER public key، ۳۲ کاراکتر hex اول،
       map کردن هر hex digit به حرف a-p) */
#define LASS_CHROME_EXT_ID "kapkhgalghmdnoojfhiameibbogfjhkj"

    FILE *f = fopen(out_path, "w");
    if (!f) return FALSE;
    fprintf(f,
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"description\": \"Lass Download Manager Native Host\",\n"
        "  \"path\": \"%s\",\n"
        "  \"type\": \"stdio\",\n"
        "  \"allowed_origins\": [\n"
        "    \"chrome-extension://" LASS_CHROME_EXT_ID "/\"\n"
        "  ],\n"
        "  \"allowed_extensions\": [\"lass@downloadmanager.local\"]\n"
        "}\n",
        LASS_EXT_HOST_NAME, escaped);
    fclose(f);
    return TRUE;
}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *browser_ext_plugin_create(ExtDownloadCb on_download,
                                            void          *userdata) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    BExt_Priv *p = (BExt_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    InitializeCriticalSection(&p->handles_lock);
    p->on_download  = on_download;
    p->userdata     = userdata;
    p->running      = TRUE;
    p->client_count = 0;

    p->server_thread = CreateThread(NULL, 0, server_thread, p, 0, NULL);
    if (!p->server_thread) {
        p->running = FALSE;
        DeleteCriticalSection(&p->lock);
        free(p); free(plug);
        return NULL;
    }

    plug->priv        = p;
    plug->name        = "browser_ext";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = NULL;
    plug->on_destroy  = NULL;

    ext_log("[BrowserExt] plugin created (pipe=%s)\n", LASS_PIPE_NAME);
    return plug;
}

void browser_ext_plugin_signal_stop(IDM_PluginHooks *plug) {
    if (!plug || !plug->priv) return;
    BExt_Priv *p = (BExt_Priv *)plug->priv;
    p->running = FALSE;
    /* دقیقاً همون trick داخل browser_ext_plugin_destroy: بدون این،
       server_thread داخل ConnectNamedPipe بلاک می‌مونه و priv->running
       رو هیچ‌وقت نمی‌بینه، چون فقط بعد از برگشتن از این تابع چک
       می‌شه. یه dummy client وصل می‌کنیم تا فوراً برگرده. امنه که
       این‌جا هم صدا زده بشه و هم بعداً در browser_ext_plugin_destroy
       (اگه اون‌موقع دیگه pipeای برای وصل شدن نباشه، CreateFileA
       ساده fail می‌شه و بی‌اثره). */
    HANDLE dummy = CreateFileA(LASS_PIPE_NAME,
                                GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);
}

void browser_ext_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    BExt_Priv *p = (BExt_Priv *)plug->priv;
    if (p) {
        p->running = FALSE;
        /* یه connection dummy برای خروج از ConnectNamedPipe */
        HANDLE dummy = CreateFileA(LASS_PIPE_NAME,
                                    GENERIC_WRITE, 0, NULL,
                                    OPEN_EXISTING, 0, NULL);
        if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);

        WaitForSingleObject(p->server_thread, 3000);
        CloseHandle(p->server_thread);

        /* منتظر exit واقعی OS-level تمام client thread ها —
           client_count==0 کافی نیست چون قبل از return 0 اتفاق
           می‌افته و thread هنوز روی stack خودشه. بدون این wait،
           ممکنه free(p) بشه ولی thread هنوز از heap استفاده کنه
           (دقیقاً علت crash های intermittent بعد از ext_destroy) */
        EnterCriticalSection(&p->handles_lock);
        int n = p->client_handle_count;
        HANDLE snapshot[EXT_MAX_CLIENTS];
        for (int i = 0; i < n; i++) snapshot[i] = p->client_handles[i];
        LeaveCriticalSection(&p->handles_lock);

        if (n > 0) {
            WaitForMultipleObjects((DWORD)n, snapshot, TRUE, 3000);
            for (int i = 0; i < n; i++) CloseHandle(snapshot[i]);
        }

        DeleteCriticalSection(&p->handles_lock);
        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}

BOOL browser_ext_register(BrowserType browsers) {
    char manifest[MAX_PATH];
    if (!write_manifest(manifest, sizeof(manifest))) return FALSE;

    BOOL ok = TRUE;
    if (browsers & BROWSER_CHROME)
        ok &= reg_set_host(
            "Software\\Google\\Chrome\\NativeMessagingHosts",
            manifest);
    if (browsers & BROWSER_FIREFOX)
        ok &= reg_set_host(
            "Software\\Mozilla\\NativeMessagingHosts",
            manifest);
    if (browsers & BROWSER_EDGE)
        ok &= reg_set_host(
            "Software\\Microsoft\\Edge\\NativeMessagingHosts",
            manifest);

    ext_log("[BrowserExt] registered (browsers=%d): %s\n",
            browsers, ok ? "OK" : "PARTIAL");

    /* ── ثبت وضعیت در رجیستری تنظیمات Lass خودمون ──
       این یه duplicate ساده از manifest path هست، فقط برای
       اینکه کاربر بتونه از همون جای تنظیمات Lass (نه دیگ مرورگر)
       ببینه extension رجیستر شده یا نه و فایل manifest کجاست.
       این جایگزین فایل JSON نمیشه — اون فایل الزام پروتکل رسمی
       مرورگرهاست و باید سرجاش بمونه، این فقط یه آینه برای UI. */
    {
        HKEY hk; DWORD disp;
        if (RegCreateKeyExA(HKEY_CURRENT_USER,
                             "Software\\Lass\\Settings\\BrowserExtension",
                             0, NULL, 0, KEY_SET_VALUE, NULL,
                             &hk, &disp) == ERROR_SUCCESS) {
            DWORD chrome_v  = (browsers & BROWSER_CHROME)  ? 1 : 0;
            DWORD firefox_v = (browsers & BROWSER_FIREFOX) ? 1 : 0;
            DWORD edge_v    = (browsers & BROWSER_EDGE)    ? 1 : 0;
            RegSetValueExA(hk, "ChromeRegistered", 0, REG_DWORD,
                           (BYTE*)&chrome_v, sizeof(chrome_v));
            RegSetValueExA(hk, "FirefoxRegistered", 0, REG_DWORD,
                           (BYTE*)&firefox_v, sizeof(firefox_v));
            RegSetValueExA(hk, "EdgeRegistered", 0, REG_DWORD,
                           (BYTE*)&edge_v, sizeof(edge_v));
            RegSetValueExA(hk, "ManifestPath", 0, REG_SZ,
                           (const BYTE*)manifest, (DWORD)(strlen(manifest)+1));
            RegCloseKey(hk);
        }
    }

    return ok;
}

BOOL browser_ext_unregister(BrowserType browsers) {
    BOOL ok = TRUE;
    if (browsers & BROWSER_CHROME)
        ok &= reg_delete_host(
            "Software\\Google\\Chrome\\NativeMessagingHosts");
    if (browsers & BROWSER_FIREFOX)
        ok &= reg_delete_host(
            "Software\\Mozilla\\NativeMessagingHosts");
    if (browsers & BROWSER_EDGE)
        ok &= reg_delete_host(
            "Software\\Microsoft\\Edge\\NativeMessagingHosts");

    /* پاک کردن tracking در رجیستری Lass */
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                       "Software\\Lass\\Settings\\BrowserExtension",
                       0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD zero = 0;
        if (browsers & BROWSER_CHROME)
            RegSetValueExA(hk, "ChromeRegistered", 0, REG_DWORD,
                           (BYTE*)&zero, sizeof(zero));
        if (browsers & BROWSER_FIREFOX)
            RegSetValueExA(hk, "FirefoxRegistered", 0, REG_DWORD,
                           (BYTE*)&zero, sizeof(zero));
        if (browsers & BROWSER_EDGE)
            RegSetValueExA(hk, "EdgeRegistered", 0, REG_DWORD,
                           (BYTE*)&zero, sizeof(zero));
        RegCloseKey(hk);
    }

    return ok;
}

BOOL browser_ext_is_registered(BrowserType browser) {
    const char *base = NULL;
    switch (browser) {
    case BROWSER_CHROME:  base="Software\\Google\\Chrome\\NativeMessagingHosts"; break;
    case BROWSER_FIREFOX: base="Software\\Mozilla\\NativeMessagingHosts"; break;
    case BROWSER_EDGE:    base="Software\\Microsoft\\Edge\\NativeMessagingHosts"; break;
    default: return FALSE;
    }
    char full[512];
    snprintf(full, sizeof(full), "%s\\%s", base, LASS_EXT_HOST_NAME);
    HKEY hKey;
    BOOL exists = (RegOpenKeyExA(HKEY_CURRENT_USER, full, 0,
                                  KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS);
    if (exists) RegCloseKey(hKey);
    return exists;
}

/* ── تشخیص کامل زنجیره‌ی native messaging ──
   برخلاف browser_ext_is_registered که فقط وجود registry key رو
   چک می‌کنه، این تابع کل زنجیره رو verify می‌کنه:
   ۱. آیا registry key هست؟
   ۲. آیا manifest.json که بهش اشاره می‌کنه واقعاً روی دیسک هست؟
   ۳. آیا مسیر "path" داخل manifest.json دقیقاً exe فعلی هست؟
      (این نکته‌ی حیاتیه برای یه برنامه‌ی پرتابل — اگه کاربر
      پوشه رو جابجا کنه یا zip جدید در مسیر دیگه‌ای extract کنه،
      manifest قدیمی هنوز به exe قدیمی اشاره می‌کنه و Chrome
      "native messaging host not found" میده بدون هیچ توضیحی
      برای کاربر که چرا)
   ۴. آیا pipe server (خود این پلاگین) در حال اجراست؟

   خروجی: یه پیام متنی قابل‌نمایش مستقیم به کاربر، به‌جای فقط
   TRUE/FALSE — چون مشکل می‌تونه در هر کدوم از این ۴ مرحله باشه
   و کاربر باید دقیقاً بدونه کجا گیر کرده. */
BOOL browser_ext_diagnose(IDM_PluginHooks *plug, BrowserType browser,
                          char *out_msg, int out_sz) {
    if (!out_msg || out_sz <= 0) return FALSE;
    out_msg[0] = '\0';

    const char *base = NULL;
    const char *bname = "Unknown";
    switch (browser) {
    case BROWSER_CHROME:  base="Software\\Google\\Chrome\\NativeMessagingHosts";  bname="Chrome";  break;
    case BROWSER_FIREFOX: base="Software\\Mozilla\\NativeMessagingHosts";        bname="Firefox"; break;
    case BROWSER_EDGE:    base="Software\\Microsoft\\Edge\\NativeMessagingHosts"; bname="Edge";    break;
    default:
        snprintf(out_msg, out_sz, "Unknown browser type.");
        return FALSE;
    }

    /* ۱. Pipe server در حال اجراست؟ */
    BOOL server_alive = FALSE;
    if (plug) {
        BExt_Priv *p = (BExt_Priv *)plug->priv;
        server_alive = (p && p->running);
    }

    /* ۲. Registry key + خوندن مسیر manifest */
    char full_key[512];
    snprintf(full_key, sizeof(full_key), "%s\\%s", base, LASS_EXT_HOST_NAME);
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, full_key, 0,
                       KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        snprintf(out_msg, out_sz,
            "%s: NOT registered.\n\n"
            "Registry key missing:\nHKCU\\%s\n\n"
            "Fix: click 'Register All' in this Settings window.",
            bname, full_key);
        return FALSE;
    }

    char manifest_path[MAX_PATH] = {0};
    DWORD msz = sizeof(manifest_path);
    LONG rr = RegQueryValueExA(hKey, "", NULL, NULL,
                               (BYTE*)manifest_path, &msz);
    RegCloseKey(hKey);

    if (rr != ERROR_SUCCESS || manifest_path[0] == '\0') {
        snprintf(out_msg, out_sz,
            "%s: Registry key exists but has no valid path value.\n\n"
            "Fix: click 'Unregister' then 'Register All' again.",
            bname);
        return FALSE;
    }

    /* ۳. فایل manifest واقعاً روی دیسک هست؟ */
    if (GetFileAttributesA(manifest_path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(out_msg, out_sz,
            "%s: Registry points to a manifest file that no longer exists:\n%s\n\n"
            "This can happen if the Lass folder was moved or deleted.\n"
            "Fix: click 'Register All' again to recreate it.",
            bname, manifest_path);
        return FALSE;
    }

    /* ۴. آیا مسیر exe داخل manifest با exe فعلی یکی هست؟
       (سناریوی حیاتی برای برنامه‌ی پرتابل: جابجا شدن پوشه) */
    char cur_exe[MAX_PATH];
    GetModuleFileNameA(NULL, cur_exe, sizeof(cur_exe));

    FILE *f = fopen(manifest_path, "r");
    BOOL path_matches = FALSE;
    if (f) {
        char buf[2048] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        /* یه چک ساده‌ی substring — کافیه چون فقط می‌خوایم بدونیم
           exe فعلی در manifest ذکر شده یا نه؛ escaping دقیق JSON
           لازم نیست چون exe فعلی رو داریم و فقط substring check
           می‌کنیم، نه parse کامل */
        if (strstr(buf, cur_exe) != NULL) path_matches = TRUE;
    }

    if (!path_matches) {
        snprintf(out_msg, out_sz,
            "%s: Manifest exists but points to a DIFFERENT Lass.exe "
            "location than the one currently running.\n\n"
            "This happens when the Lass folder is moved after "
            "registration (common for portable apps).\n\n"
            "Current Lass.exe:\n%s\n\n"
            "Fix: click 'Register All' again to update the path.",
            bname, cur_exe);
        return FALSE;
    }

    /* همه‌چیز درسته */
    snprintf(out_msg, out_sz,
        "%s: Registration looks correct.\n\n"
        "Manifest: %s\n"
        "Pipe server: %s\n\n"
        "If the extension still shows 'Lass is not running', make sure:\n"
        "- Lass is actually running (check system tray)\n"
        "- The extension's ID in chrome://extensions matches the one "
        "expected (see INSTALL.md)",
        bname, manifest_path,
        server_alive ? "running" : "NOT running (restart Lass)");

    return path_matches && server_alive;
}

/* ── تشخیص نصب بودن مرورگر ──
   با خوندن مسیر exe از رجیستری چک می‌کنیم.
   این اطلاعات در HKCU یا HKLM ذخیره شدن. */
BOOL browser_ext_is_browser_installed(BrowserType browser) {
    /* کلیدهای رجیستری مرورگرها برای مسیر exe */
    const char *keys[] = {
        NULL, /* placeholder */
        "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe",   /* CHROME */
        "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\firefox.exe",  /* FIREFOX */
        "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe"    /* EDGE */
    };

    int idx = 0;
    if (browser == BROWSER_CHROME)  idx = 1;
    if (browser == BROWSER_FIREFOX) idx = 2;
    if (browser == BROWSER_EDGE)    idx = 3;
    if (idx == 0) return FALSE;

    /* ابتدا HKLM چک کن (معمول)، بعد HKCU (نصب per-user) */
    HKEY hk;
    BOOL found = FALSE;

    const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2 && !found; i++) {
        if (RegOpenKeyExA(roots[i], keys[idx], 0,
                          KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            char path[MAX_PATH] = {0};
            DWORD sz = sizeof(path);
            if (RegQueryValueExA(hk, "", NULL, NULL,
                                  (BYTE *)path, &sz) == ERROR_SUCCESS && path[0])
                found = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
            RegCloseKey(hk);
        }
    }
    return found;
}

/* مسیر پوشه extensions کنار Lass.exe */
void browser_ext_get_extension_dir(char *out, int out_sz) {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *bs = strrchr(exe, '\\');
    if (bs) *(bs + 1) = '\0';
    snprintf(out, out_sz, "%sextensions", exe);
}

BOOL browser_ext_is_running(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    BExt_Priv *p = (BExt_Priv *)plug->priv;
    return p ? p->running : FALSE;
}

int browser_ext_client_count(IDM_PluginHooks *plug) {
    if (!plug) return 0;
    BExt_Priv *p = (BExt_Priv *)plug->priv;
    return p ? p->client_count : 0;
}

long browser_ext_messages_received(IDM_PluginHooks *plug) {
    if (!plug) return 0;
    BExt_Priv *p = (BExt_Priv *)plug->priv;
    return p ? p->messages_received : 0;
}
