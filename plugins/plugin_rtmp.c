/* plugin_rtmp.c
   ─────────────────────────────────────────────────────────────
   RTMP Plugin  –  ffmpeg/rtmpdump backend

   جریان:
     1. rtmp_detect_backend: پیدا کردن ffmpeg یا rtmpdump در PATH
     2. rtmp_download: اجرای process با CreateProcess
     3. نظارت بر stdout/stderr برای progress
     4. WM_APP notification به GUI وقتی تموم شد
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/url_secure.h"
#include "plugin_rtmp.h"
#include "../src/types.h"
#include "../src/lass_log.h"
#include "../src/shutdown_brake.h"

/* WM_APP messages برای progress */
#define RTMP_WM_PROGRESS (WM_APP + 60)
#define RTMP_WM_DONE     (WM_APP + 61)

typedef struct {
    DWORD        magic;          /* canary برای تشخیص corruption حافظه —
                                     اگه این مقدار درست نباشه یعنی یه‌جای
                                     دیگه به این بلوک حافظه نوشته شده */
    RtmpConfig   cfg;
    HANDLE       proc_handle;    /* handle process در حال اجرا */
    HANDLE       proc_thread;
    volatile BOOL downloading;
    volatile BOOL cancel_flag;
    char         last_error[256];
    HWND         notify_hwnd;
    CRITICAL_SECTION lock;
} Rtmp_Priv;

#define RTMP_PRIV_MAGIC 0x52544D50u /* "RTMP" ASCII */

/* ── جستجو در PATH ── */
/* ── جستجو در PATH ──
   به‌جای الگوی دو-فراخوانی GetEnvironmentVariableA (size query +
   fetch) که می‌تونه edge-case داشته باشه، یه بافر heap با اندازه‌ی
   ثابت و کافی استفاده می‌کنیم. PATH روی ویندوز عملاً به ندرت از
   16KB بیشتر میشه. بافر local (نه static) تا thread-safe باشه —
   این تابع ممکنه از worker thread دانلود هم صدا زده بشه. */
#define RTMP_PATH_BUF_SIZE 16384

static BOOL find_in_path(const char *exe, char *out, int out_sz) {
    if (!exe || !out || out_sz <= 0) return FALSE;
    out[0] = '\0';

    char *paths = (char *)calloc(RTMP_PATH_BUF_SIZE, 1);
    if (!paths) return FALSE;

    DWORD got = GetEnvironmentVariableA("PATH", paths, RTMP_PATH_BUF_SIZE);
    /* got == 0 یعنی متغیر وجود نداره؛ got >= RTMP_PATH_BUF_SIZE یعنی
       truncate شده — هر دو حالت رو امن رد می‌کنیم */
    if (got == 0 || got >= RTMP_PATH_BUF_SIZE) { free(paths); return FALSE; }

    BOOL   found   = FALSE;
    size_t exe_len = strlen(exe);
    if (exe_len == 0 || exe_len >= MAX_PATH) { free(paths); return FALSE; }

    char *seg_start = paths;
    char *p = paths;
    while (!found) {
        if (*p == ';' || *p == '\0') {
            size_t seg_len = (size_t)(p - seg_start);
            if (seg_len > 0 && seg_len + 1 + exe_len < (size_t)MAX_PATH) {
                char full[MAX_PATH];
                memset(full, 0, sizeof(full));
                memcpy(full, seg_start, seg_len);
                full[seg_len] = '\\';
                memcpy(full + seg_len + 1, exe, exe_len);
                full[seg_len + 1 + exe_len] = '\0';

                if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) {
                    strncpy(out, full, (size_t)out_sz - 1);
                    out[out_sz - 1] = '\0';
                    found = TRUE;
                }
            }
            if (*p == '\0') break;
            seg_start = p + 1;
        }
        p++;
        /* محافظت اضافی در برابر هر سناریوی غیرمنتظره */
        if ((size_t)(p - paths) > (size_t)got + 1) break;
    }
    free(paths);
    if (found) return TRUE;

    /* بررسی کنار exe */
    char self[MAX_PATH];
    memset(self, 0, sizeof(self));
    DWORD self_len = GetModuleFileNameA(NULL, self, MAX_PATH);
    if (self_len == 0 || self_len >= MAX_PATH) return FALSE;

    char *bs = strrchr(self, '\\');
    if (bs) {
        *(bs+1) = '\0';
        size_t dir_len = strlen(self);
        if (dir_len + exe_len < (size_t)MAX_PATH) {
            char full[MAX_PATH];
            memset(full, 0, sizeof(full));
            snprintf(full, sizeof(full), "%s%s", self, exe);
            if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) {
                strncpy(out, full, (size_t)out_sz - 1);
                out[out_sz - 1] = '\0';
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ======================================================
   Public API
   ====================================================== */
BOOL rtmp_is_rtmp_url(const char *url) {
    if (!url) return FALSE;
    return (_strnicmp(url, "rtmp://",  7) == 0 ||
            _strnicmp(url, "rtmps://", 8) == 0 ||
            _strnicmp(url, "rtmpe://", 8) == 0 ||
            _strnicmp(url, "rtmpt://", 8) == 0 ||
            _strnicmp(url, "rtmpte://",9) == 0);
}

RtmpConfig rtmp_detect_backend(void) {
    RtmpConfig cfg = {0};
    cfg.backend = RTMP_BACKEND_NONE;
    cfg.found   = FALSE;

    /* ffmpeg اول */
    if (find_in_path("ffmpeg.exe", cfg.tool_path, sizeof(cfg.tool_path))) {
        cfg.backend = RTMP_BACKEND_FFMPEG;
        cfg.found   = TRUE;
        LOGI("rtmp", "Found ffmpeg: %s", cfg.tool_path);
        return cfg;
    }

    /* rtmpdump */
    if (find_in_path("rtmpdump.exe", cfg.tool_path, sizeof(cfg.tool_path))) {
        cfg.backend = RTMP_BACKEND_RTMPDUMP;
        cfg.found   = TRUE;
        LOGI("rtmp", "Found rtmpdump: %s", cfg.tool_path);
        return cfg;
    }

    LOGW("rtmp", "No backend found (install ffmpeg or rtmpdump)");
    return cfg;
}

/* ── worker thread برای اجرای process ── */
typedef struct {
    Rtmp_Priv  *priv;
    char        url[2048];
    char        out_path[MAX_PATH * 2];
} RtmpWorker;

static DWORD WINAPI rtmp_worker_thread_impl(LPVOID param) {
    RtmpWorker *w = (RtmpWorker *)param;
    Rtmp_Priv  *p = w->priv;

    /* ساخت command line */
    /* حیاتی: هم w->url و هم w->out_path بدون validation داخل
       command line قرار می‌گیرن که به CreateProcessA داده میشه.
       اگه هرکدوم شامل کاراکتر " باشه، می‌تونه از quote خارج بشه
       و command injection بکنه. یه URL واقعی هیچ‌وقت quote خام
       نداره، و out_path هم چون از filename تازه sanitize‌شده
       ساخته میشه نباید quote داشته باشه — ولی این چک دوم
       defense-in-depth هست، نه فرض اعتماد به لایه‌های قبلی. */
    if (strchr(w->url, '"') || strchr(w->out_path, '"')) {
        LOGE("rtmp", "Rejected process launch: URL or path contains quote "
             "character (potential command injection attempt)");
        snprintf(p->last_error, sizeof(p->last_error),
                 "Invalid URL or path (contains illegal character)");
        return 1;
    }

    char cmd[4096] = {0};
    if (p->cfg.backend == RTMP_BACKEND_FFMPEG) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -i \"%s\" -c copy -y \"%s\"",
                 p->cfg.tool_path, w->url, w->out_path);
    } else {
        /* rtmpdump */
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -r \"%s\" -o \"%s\"",
                 p->cfg.tool_path, w->url, w->out_path);
    }

    LOGI("rtmp", "Running: %s", cmd);

    /* redirect stdout/stderr */
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE hRead = NULL, hWrite = NULL;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;

    PROCESS_INFORMATION pi = {0};
    BOOL created = CreateProcessA(NULL, cmd, NULL, NULL,
                                   TRUE, CREATE_NO_WINDOW,
                                   NULL, NULL, &si, &pi);
    CloseHandle(hWrite);

    if (!created) {
        snprintf(p->last_error, sizeof(p->last_error),
                 "Failed to start %s",
                 p->cfg.backend == RTMP_BACKEND_FFMPEG
                 ? "ffmpeg" : "rtmpdump");
        p->downloading = FALSE;
        if (p->notify_hwnd)
            PostMessageA(p->notify_hwnd, RTMP_WM_DONE, 0, 0);
        CloseHandle(hRead);
        free(w);
        return 1;
    }

    EnterCriticalSection(&p->lock);
    p->proc_handle = pi.hProcess;
    p->proc_thread = pi.hThread;
    LeaveCriticalSection(&p->lock);

    /* خواندن output */
    char buf[512];
    DWORD read_bytes = 0;
    while (!p->cancel_flag) {
        BOOL ok = ReadFile(hRead, buf, sizeof(buf)-1, &read_bytes, NULL);
        if (!ok || read_bytes == 0) break;
        buf[read_bytes] = '\0';
        /* progress parse از ffmpeg: "frame=..." یا "size=..." */
        if (p->notify_hwnd && strstr(buf, "size="))
            PostMessageA(p->notify_hwnd, RTMP_WM_PROGRESS, 0, 0);
    }

    /* اگه cancel شد، process رو kill کن */
    if (p->cancel_flag) {
        TerminateProcess(pi.hProcess, 1);
        LOGI("rtmp", "Cancelled");
    }

    DWORD exit_code = 1;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    p->proc_handle = NULL;
    p->proc_thread = NULL;
    p->downloading = FALSE;

    LOGI("rtmp", "Process exited with code %lu", exit_code);

    if (p->notify_hwnd)
        PostMessageA(p->notify_hwnd, RTMP_WM_DONE,
                     (WPARAM)(exit_code == 0 ? 1 : 0), 0);

    free(w);
    return exit_code;
}

/* wrapper نازک — همون الگوی launch_thread در engine.c؛ بدون
   دستکاری ۳ نقطه‌ی return داخل تابع اصلی */
static DWORD WINAPI rtmp_worker_thread(LPVOID param) {
    int brake_id = shutdown_brake_register("rtmp_worker");
    DWORD result = rtmp_worker_thread_impl(param);
    shutdown_brake_unregister(brake_id);
    return result;
}

IDM_Result rtmp_download(IDM_PluginHooks *plug,
                          const char *url,
                          const char *out_path,
                          HWND notify_hwnd) {
    if (!plug || !url || !out_path) return IDM_ERR_INVALID;
    Rtmp_Priv *p = (Rtmp_Priv *)plug->priv;
    if (!p) return IDM_ERR_INVALID;

    if (p->downloading) return IDM_ERR_INVALID; /* قبلاً در حال دانلود */

    if (!p->cfg.found) {
        /* تلاش مجدد برای پیدا کردن */
        p->cfg = rtmp_detect_backend();
        if (!p->cfg.found) {
            if (notify_hwnd)
                MessageBoxA(notify_hwnd,
                    "RTMP download requires ffmpeg or rtmpdump.\n\n"
                    "Please install ffmpeg and add it to PATH:\n"
                    "  https://ffmpeg.org/download.html\n\n"
                    "Then restart Lass.",
                    "RTMP - Backend Not Found",
                    MB_ICONWARNING);
            return IDM_ERR_INVALID;
        }
    }

    p->notify_hwnd = notify_hwnd;
    p->cancel_flag = FALSE;
    p->downloading = TRUE;

    RtmpWorker *w = (RtmpWorker *)malloc(sizeof(*w));
    if (!w) { p->downloading = FALSE; return IDM_ERR_INVALID; }

    strncpy(w->url,      url,      sizeof(w->url)     - 1);
    strncpy(w->out_path, out_path, sizeof(w->out_path)- 1);
    w->priv = p;

    HANDLE t = CreateThread(NULL, 0, rtmp_worker_thread, w, 0, NULL);
    if (!t) {
        free(w); p->downloading = FALSE;
        return IDM_ERR_INVALID;
    }
    CloseHandle(t);

    char red_url[256];
    url_redact_for_log(url, red_url, sizeof(red_url));
    LOGI("rtmp", "Download started: %s", red_url);
    return IDM_OK;
}

BOOL rtmp_is_downloading(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    Rtmp_Priv *p = (Rtmp_Priv *)plug->priv;
    return p ? p->downloading : FALSE;
}

void rtmp_cancel(IDM_PluginHooks *plug) {
    if (!plug) return;
    Rtmp_Priv *p = (Rtmp_Priv *)plug->priv;
    if (!p) return;
    p->cancel_flag = TRUE;
}

/* ── plugin hooks ── */
static int  rtmp_on_init   (void) { return 0; }
static void rtmp_on_destroy(void) {}

IDM_PluginHooks *rtmp_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    Rtmp_Priv *p = (Rtmp_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->magic = RTMP_PRIV_MAGIC;
    p->cfg = rtmp_detect_backend();

    plug->priv        = p;
    plug->name        = "rtmp_handler";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = rtmp_on_init;
    plug->on_destroy  = rtmp_on_destroy;

    LOGI("rtmp", "plugin created (backend=%s)",
         p->cfg.found ?
         (p->cfg.backend == RTMP_BACKEND_FFMPEG ? "ffmpeg" : "rtmpdump")
         : "none");
    return plug;
}

void rtmp_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    Rtmp_Priv *p = (Rtmp_Priv *)plug->priv;
    if (p) {
        if (p->magic != RTMP_PRIV_MAGIC) {
            LOGE("rtmp",
                 "FATAL: Rtmp_Priv magic mismatch "
                 "(got 0x%08lX, expected 0x%08lX) — "
                 "heap corruption detected, skipping cleanup",
                 (unsigned long)p->magic,
                 (unsigned long)RTMP_PRIV_MAGIC);
            free(plug);
            return;
        }
        if (p->downloading) rtmp_cancel(plug);
        Sleep(200);
        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}
