/* lass_log.c — Thread-safe logger with rotation */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "lass_log.h"

static struct {
    CRITICAL_SECTION lock;
    HANDLE           hFile;
    char             path[MAX_PATH];
    LogLevel         min_level;
    BOOL             initialized;
    LONGLONG         bytes_written;
} g_log;

/* ── قفل مستقل برای خروجی کنسول (stdout/stderr) ──
   مشکل قبلی: log_write بدون هیچ قفلی به stderr می‌نوشت (فقط
   نوشتن در فایل قفل داشت)، پس وقتی چند thread همزمان لاگ
   می‌زدن یا یه thread داشت لاگ می‌زد و thread اصلی (مثلاً در
   test_all.c) همزمان با printf به stdout می‌نوشت، کاراکترها
   به‌هم می‌ریختن (چون stdout/stderr هر دو به یه کنسول واحد
   میرن). این قفل مستقل از g_log.lock هست چون g_log.lock فقط
   داخل log_init() ساخته میشه — اگه log_init هیچ‌وقت صدا زده
   نشه (مثلاً در بعضی مسیرهای تست)، g_log.lock مقداردهی‌نشده
   می‌مونه و استفاده ازش امن نیست. این یکی با الگوی
   double-checked lazy-init همیشه امنه، حتی بدون فراخوانی
   قبلی. */
static CRITICAL_SECTION g_console_lock;
static volatile LONG    g_console_lock_state = 0; /* 0=none,1=init-in-progress,2=ready */

static void ensure_console_lock(void) {
    if (g_console_lock_state == 2) return;
    if (InterlockedCompareExchange(&g_console_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_console_lock);
        MemoryBarrier();
        InterlockedExchange(&g_console_lock_state, 2);
    } else {
        while (g_console_lock_state < 2) Sleep(0);
    }
}

void log_console_lock(void) {
    ensure_console_lock();
    EnterCriticalSection(&g_console_lock);
}

void log_console_unlock(void) {
    LeaveCriticalSection(&g_console_lock);
}

static const char *level_name(LogLevel lv) {
    switch (lv) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "?????";
    }
}

static void do_rotate(void) {
    if (!g_log.path[0]) return;
    if (g_log.hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log.hFile);
        g_log.hFile = INVALID_HANDLE_VALUE;
    }
    char buf[MAX_PATH + 8];
    snprintf(buf, sizeof(buf), "%s.%d", g_log.path, LOG_MAX_BACKUPS);
    DeleteFileA(buf);
    for (int i = LOG_MAX_BACKUPS - 1; i >= 1; i--) {
        char src[MAX_PATH+16], dst[MAX_PATH+16];
        int avail = (int)sizeof(src) - 8;
        snprintf(src, sizeof(src), "%.*s.%d", avail, g_log.path, i);
        snprintf(dst, sizeof(dst), "%.*s.%d", avail, g_log.path, i+1);
        MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING);
    }
    snprintf(buf, sizeof(buf), "%s.1", g_log.path);
    MoveFileExA(g_log.path, buf, MOVEFILE_REPLACE_EXISTING);
    g_log.hFile = CreateFileA(g_log.path, GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH, NULL);
    g_log.bytes_written = 0;
    if (g_log.hFile != INVALID_HANDLE_VALUE) {
        const char *hdr = "=== Lass Log (rotated) ===\n";
        DWORD w; WriteFile(g_log.hFile, hdr, (DWORD)strlen(hdr), &w, NULL);
    }
}

void log_init(const char *log_path, LogLevel min_level) {
    if (g_log.initialized) return;
    InitializeCriticalSection(&g_log.lock);
    g_log.min_level   = min_level;
    g_log.hFile       = INVALID_HANDLE_VALUE;
    g_log.initialized = TRUE;
    g_log.bytes_written = 0;
    memset(g_log.path, 0, sizeof(g_log.path));

    if (log_path && log_path[0]) {
        strncpy(g_log.path, log_path, MAX_PATH-1);
        g_log.hFile = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH, NULL);
        if (g_log.hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz = {0};
            GetFileSizeEx(g_log.hFile, &sz);
            g_log.bytes_written = sz.QuadPart;
            SetFilePointer(g_log.hFile, 0, NULL, FILE_END);
            if (g_log.bytes_written >= LOG_MAX_SIZE) do_rotate();
            const char *sep = "\n=== Lass Session Start ===\n";
            DWORD w; WriteFile(g_log.hFile, sep, (DWORD)strlen(sep), &w, NULL);
            g_log.bytes_written += w;
        }
    }
    LOGI(LOG_MOD_CORE, "Lass Log v5 initialized (level=%s path=%s)",
         level_name(min_level), log_path ? log_path : "stderr-only");
}

void log_destroy(void) {
    if (!g_log.initialized) return;
    LOGI(LOG_MOD_CORE, "Log shutting down");
    EnterCriticalSection(&g_log.lock);
    if (g_log.hFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log.hFile);
        CloseHandle(g_log.hFile);
        g_log.hFile = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_log.lock);
    DeleteCriticalSection(&g_log.lock);
    g_log.initialized = FALSE;
}

void log_set_level(LogLevel level) { g_log.min_level = level; }

void log_flush(void) {
    if (!g_log.initialized) return;
    EnterCriticalSection(&g_log.lock);
    if (g_log.hFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log.hFile);
    LeaveCriticalSection(&g_log.lock);
}

void log_write(LogLevel level, const char *module, const char *fmt, ...) {
    if (level < g_log.min_level) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char body[LOG_MAX_LINE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[LOG_MAX_LINE + 80];
    int len = snprintf(line, sizeof(line),
        "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] [%-12s] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        level_name(level), module ? module : "?", body);
    if (len <= 0) return;
    if (len >= (int)sizeof(line)) { line[sizeof(line)-2]='\n'; len=(int)sizeof(line)-1; }
    log_console_lock();
    fputs(line, stderr);
    log_console_unlock();
    if (!g_log.initialized || g_log.hFile == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_log.lock);
    DWORD w = 0;
    WriteFile(g_log.hFile, line, (DWORD)len, &w, NULL);
    g_log.bytes_written += w;
    if (g_log.bytes_written >= LOG_MAX_SIZE) do_rotate();
    LeaveCriticalSection(&g_log.lock);
}
