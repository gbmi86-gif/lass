/* queue_persist.c — ذخیره/بارگذاری لیست دانلودها
   ─────────────────────────────────────────────────
   ایده: مثل journaling سیستم‌های فایل — هر بار که
   وضعیت queue تغییر می‌کنه، یه snapshot کامل به
   queue.dat نوشته میشه. در startup، این فایل خونده
   میشه و دانلودهای قبلی بازیابی میشن.

   چرا snapshot کامل به‌جای append-only log؟
   - ساده‌تر و reliable‌تر
   - فایل همیشه consistent هست (نه partially-written)
   - اگه برق بره بین دو write، در نهایت یا فایل قدیمی
     کاملاً درست داریم یا فایل جدید کاملاً درست
   ─────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "queue_persist.h"
#include "engine.h"
#include "lass_log.h"

/* Header فایل */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    uint32_t count;
    uint32_t _pad2;
} QueueFileHeader;

void queue_persist_get_path(char *out, int out_sz) {
    char appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
        snprintf(out, out_sz, "%s\\Lass\\queue.dat", appdata);
    else
        snprintf(out, out_sz, "queue.dat");
}

BOOL queue_persist_save(IDM_Engine *eng, const char *queue_path) {
    if (!eng || !queue_path) return FALSE;

    /* جمع‌آوری رکوردها زیر lock */
    EnterCriticalSection(&eng->lock);

    /* شمارش تعداد */
    uint32_t count = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next)
        count++;

    QueueRecord *records = (QueueRecord *)calloc(count, sizeof(QueueRecord));
    if (!records && count > 0) {
        LeaveCriticalSection(&eng->lock);
        return FALSE;
    }

    uint32_t i = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next, i++) {
        QueueRecord *r = &records[i];
        r->id           = dl->id;
        r->total_size   = dl->total_size;
        r->downloaded   = (uint64_t)dl->downloaded;
        r->state        = (uint32_t)dl->state;
        r->num_segments = dl->num_segments;
        r->group_id     = dl->group_id;
        r->has_schedule = dl->has_schedule;
        r->sched_low    = dl->scheduled_at.dwLowDateTime;
        r->sched_high   = dl->scheduled_at.dwHighDateTime;
        strncpy(r->url,        dl->url,        IDM_MAX_URL      - 1);
        strncpy(r->out_path,   dl->out_path,   IDM_MAX_PATH     - 1);
        strncpy(r->filename,   dl->filename,   IDM_MAX_FILENAME - 1);
        strncpy(r->group_name, dl->group_name, 63);
    }

    LeaveCriticalSection(&eng->lock);

    /* نوشتن به یه فایل temp اول (atomic write pattern):
       - نوشتن به queue.dat.tmp
       - بعد rename به queue.dat
       اگه برنامه وسط نوشتن کرش کنه، فایل اصلی دست‌نخورده‌ست */
    char tmp_path[MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", queue_path);

    HANDLE hf = CreateFileA(tmp_path,
                             GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        free(records);
        return FALSE;
    }

    QueueFileHeader hdr = {0};
    hdr.magic   = QUEUE_PERSIST_MAGIC;
    hdr.version = QUEUE_PERSIST_VERSION;
    hdr.count   = count;

    DWORD written;
    BOOL ok = WriteFile(hf, &hdr, sizeof(hdr), &written, NULL);
    if (ok && count > 0)
        ok = WriteFile(hf, records, count * sizeof(QueueRecord), &written, NULL);

    FlushFileBuffers(hf);
    CloseHandle(hf);
    free(records);

    if (!ok) {
        DeleteFileA(tmp_path);
        return FALSE;
    }

    /* atomic rename */
    MoveFileExA(tmp_path, queue_path, MOVEFILE_REPLACE_EXISTING);
    return TRUE;
}

BOOL queue_persist_load(IDM_Engine *eng, const char *queue_path) {
    if (!eng || !queue_path) return FALSE;

    HANDLE hf = CreateFileA(queue_path,
                             GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;

    QueueFileHeader hdr = {0};
    DWORD read_bytes;
    if (!ReadFile(hf, &hdr, sizeof(hdr), &read_bytes, NULL) ||
        read_bytes != sizeof(hdr) ||
        hdr.magic   != QUEUE_PERSIST_MAGIC ||
        hdr.version != QUEUE_PERSIST_VERSION ||
        hdr.count   == 0 ||
        hdr.count   > 10000) { /* sanity check */
        CloseHandle(hf);
        return FALSE;
    }

    QueueRecord *records = (QueueRecord *)calloc(hdr.count, sizeof(QueueRecord));
    if (!records) { CloseHandle(hf); return FALSE; }

    BOOL ok = ReadFile(hf, records, hdr.count * sizeof(QueueRecord),
                       &read_bytes, NULL);
    CloseHandle(hf);

    if (!ok || read_bytes != hdr.count * sizeof(QueueRecord)) {
        free(records);
        return FALSE;
    }

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        QueueRecord *r = &records[i];

        /* فقط دانلودهایی رو بازیابی کن که منطقی هستن:
           - URL معتبر داشته باشن
           - فایل .tels یا فایل کامل روی دیسک باشه
           - در حالت completed/cancelled/failed/queued/paused باشن */
        if (!r->url[0] || !r->out_path[0]) continue;

        /* چک کن آیا فایل .tels یا فایل نهایی وجود داره */
        char tels_path[MAX_PATH] = {0};
        int avail = MAX_PATH - 6;
        snprintf(tels_path, MAX_PATH, "%.*s.tels", avail, r->out_path);

        BOOL has_final = (GetFileAttributesA(r->out_path) != INVALID_FILE_ATTRIBUTES);

        /* تعیین state مناسب برای بازیابی:
           - همه دانلودهای ناقص/در-صف به DL_PAUSED تبدیل میشن
             تا کاربر خودش تصمیم بگیره Resume کنه
           - این از auto-launch شدن دانلودهای ناقص جلوگیری می‌کنه
             (که در network نبود یا پس از کرش می‌تونه مشکل‌ساز باشه) */
        DL_State restore_state;
        if (r->state == DL_COMPLETED && has_final) {
            restore_state = DL_COMPLETED;
        } else if (r->state == DL_CANCELLED) {
            restore_state = DL_CANCELLED;
        } else {
            /* همه‌ی حالت‌های دیگه (QUEUED, PAUSED, FAILED, LAUNCHING)
               رو به PAUSED تبدیل کن — کاربر Resume می‌زنه */
            restore_state = DL_PAUSED;
        }

        /* اضافه کردن به engine بدون شروع خودکار */
        IDM_Download *dl = engine_add(eng, r->url, r->out_path,
                                       r->num_segments ? r->num_segments : 8, 0);
        if (!dl) continue;

        /* آپدیت فیلدهای اضافی — state رو بعد از add تنظیم کن
           چون engine_add state رو DL_QUEUED می‌کنه.
           نکته مهم: engine_add هیچ‌وقت active_count رو افزایش
           نمیده (فقط scheduler اون رو هنگام launch افزایش میده)
           پس نباید اینجا active_count رو کم کنیم. */
        EnterCriticalSection(&eng->lock);
        dl->state      = restore_state;
        dl->total_size = r->total_size;
        dl->downloaded = (LONGLONG)r->downloaded;
        strncpy(dl->filename,   r->filename,   IDM_MAX_FILENAME - 1);
        strncpy(dl->group_name, r->group_name, sizeof(dl->group_name) - 1);
        dl->group_id     = r->group_id;
        dl->has_schedule = r->has_schedule;
        if (dl->has_schedule) {
            dl->scheduled_at.dwLowDateTime  = r->sched_low;
            dl->scheduled_at.dwHighDateTime = r->sched_high;
        }
        LeaveCriticalSection(&eng->lock);

        loaded++;
    }

    free(records);
    LOGI(LOG_MOD_QUEUE, "Loaded %u/%u downloads from %s",
         loaded, hdr.count, queue_path);
    return loaded > 0;
}
