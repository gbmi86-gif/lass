/* storage.c  –  Windows HANDLE-based random-access file writer
   Uses OVERLAPPED so multiple segments can write concurrently
   without a mutex per byte.                                      */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage.h"

struct IDM_FileWriter {
    HANDLE    hFile;
    CRITICAL_SECTION cs;       /* protects hFile for non-overlapped seeks */
    uint64_t  expected_size;
    char      path[IDM_MAX_PATH];
};

/* ======================================================
   Open / create file
   ====================================================== */
IDM_FileWriter *fw_open(const char *path, uint64_t expected_size, bool prealloc) {
    if (!path) return NULL;

    IDM_FileWriter *fw = (IDM_FileWriter *)calloc(1, sizeof(*fw));
    if (!fw) return NULL;

    snprintf(fw->path, IDM_MAX_PATH, "%s", path);
    fw->expected_size = expected_size;
    InitializeCriticalSection(&fw->cs);

    /* Try to open existing file for resume; otherwise create
       FILE_SHARE_READ|WRITE: اجازه میده shell و explorer فایل رو ببینن
       FILE_FLAG_OVERLAPPED: حیاتی — بدون این فلگ، Windows فایل رو
       synchronous در نظر می‌گیره و OVERLAPPED.Offset/OffsetHigh رو
       به درستی برای هر thread رعایت نمی‌کنه. نتیجه‌اش race condition
       بین segment-thread های مختلف بود که می‌تونست باعث نوشتن بایت‌ها
       در آفست اشتباه بشه — این دقیقاً چیزی بود که فایل‌های ZIP رو
       (که به دقت بایت در انتهای فایل حساسن) خراب می‌کرد، در حالی که
       فایل‌های ویدیویی بزرگ‌تر کمتر این مشکل قابل توجه بود. */
    fw->hFile = CreateFileA(path,
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            NULL);

    if (fw->hFile == INVALID_HANDLE_VALUE) {
        DeleteCriticalSection(&fw->cs);
        free(fw);
        return NULL;
    }

    /* Pre-allocate disk space to avoid fragmentation */
    if (prealloc && expected_size > 0) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)expected_size;
        if (SetFilePointerEx(fw->hFile, li, NULL, FILE_BEGIN))
            SetEndOfFile(fw->hFile);
        /* Reset pointer */
        li.QuadPart = 0;
        SetFilePointerEx(fw->hFile, li, NULL, FILE_BEGIN);
    }

    return fw;
}

/* ======================================================
   Write at arbitrary offset (thread-safe via OVERLAPPED)
   ====================================================== */
int fw_write(IDM_FileWriter *fw, uint64_t offset, const void *data, size_t len) {
    if (!fw || !data || len == 0) return -1;
    if (fw->hFile == INVALID_HANDLE_VALUE) return -1;

    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    /* event مستقل برای این فراخوانی — حیاتی برای صحت overlapped I/O
       وقتی چند thread همزمان روی همون hFile کار می‌کنن. بدون این،
       GetOverlappedResult می‌تونه signal یه عملیات دیگه رو دریافت
       کنه و نتیجه‌ی اشتباه برگردونه (corruption احتمالی). */
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    DWORD written = 0;
    BOOL ok = WriteFile(fw->hFile, data, (DWORD)len, &written, &ov);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(fw->hFile, &ov, &written, TRUE);
        }
    }

    CloseHandle(ov.hEvent);
    return (ok && written == (DWORD)len) ? 0 : -1;
}

/* ======================================================
   Read at arbitrary offset
   ====================================================== */
int fw_read(IDM_FileWriter *fw, uint64_t offset, void *buf, size_t len) {
    if (!fw || !buf || len == 0) return -1;
    if (fw->hFile == INVALID_HANDLE_VALUE) return -1;

    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    DWORD read_bytes = 0;
    BOOL  ok       = ReadFile(fw->hFile, buf, (DWORD)len, &read_bytes, &ov);
    BOOL  eof_zero = FALSE;

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
            ok = GetOverlappedResult(fw->hFile, &ov, &read_bytes, TRUE);
        else if (err == ERROR_HANDLE_EOF)
            eof_zero = TRUE;
    }

    CloseHandle(ov.hEvent);

    if (eof_zero) return 0;
    return (ok && read_bytes == (DWORD)len) ? 0 : -1;
}

/* ======================================================
   Flush to disk
   ====================================================== */
void fw_sync(IDM_FileWriter *fw) {
    if (!fw || fw->hFile == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(fw->hFile);
}

/* ======================================================
   Get current file size
   ====================================================== */
uint64_t fw_size(IDM_FileWriter *fw) {
    if (!fw || fw->hFile == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz = {0};
    GetFileSizeEx(fw->hFile, &sz);
    return (uint64_t)sz.QuadPart;
}

/* ======================================================
   Close and free
   ====================================================== */
void fw_close(IDM_FileWriter *fw) {
    if (!fw) return;
    if (fw->hFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(fw->hFile);
        CloseHandle(fw->hFile);
    }
    DeleteCriticalSection(&fw->cs);
    free(fw);
}
