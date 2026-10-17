/* queue_persist.c — ذخیره و بارگذاری صف دانلودها روی container .tels

   صف داخل فروشگاه برنامه (پوشه‌ی "queue"، نام "state") نگه داشته می‌شود. هر بار که
   وضعیت صف تغییر می‌کند، یک snapshot کامل جایگزین می‌شود؛ جایگزینی اتمیک است
   (رجوع کن به lass_store.c).

   queue.dat قدیمی فقط برای مهاجرت یک‌باره خوانده می‌شود، آن هم اگر فروشگاه صفی
   نداشته باشد. فایل قدیمی دست‌نخورده می‌ماند. */
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
#include "lass_store.h"
#include "lass_paths.h"
#include "lass_log.h"

#define QUEUE_STORE_DIR   "queue"
#define QUEUE_STORE_NAME  "state"
#define QUEUE_LEGACY_MAX  (16u * 1024u * 1024u)

/* سرآیند بلاب صف؛ همان قالب فایل قدیمی */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    uint32_t count;
    uint32_t _pad2;
} QueueFileHeader;


void queue_persist_get_path(char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = '\0';
    char dir[MAX_PATH] = {0};
    if (!lass_data_dir(dir, sizeof(dir))) return;
    const char *name = "\\queue.dat";
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + nl + 1 > (size_t)out_sz) return;
    memcpy(out, dir, dl);
    memcpy(out + dl, name, nl + 1);
}

BOOL queue_persist_save(IDM_Engine *eng) {
    if (!eng) return FALSE;

    /* جمع‌آوری رکوردها زیر lock */
    EnterCriticalSection(&eng->lock);

    uint32_t count = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next)
        count++;

    QueueRecordV2 *records = (QueueRecordV2 *)calloc(count ? count : 1u, sizeof(QueueRecordV2));
    if (!records) {
        LeaveCriticalSection(&eng->lock);
        return FALSE;
    }

    uint32_t i = 0;
    for (IDM_Download *dl = eng->queue_head; dl && i < count; dl = dl->next, i++) {
        QueueRecord *r = &records[i].base;
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
        strncpy(records[i].pass_enc, dl->pass_enc, sizeof(records[i].pass_enc) - 1);
    }

    LeaveCriticalSection(&eng->lock);

    QueueFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = QUEUE_PERSIST_MAGIC;
    hdr.version = QUEUE_PERSIST_VERSION;
    hdr.count   = count;

    size_t body  = (size_t)count * sizeof(QueueRecordV2);
    size_t total = sizeof(hdr) + body;
    uint8_t *blob = (uint8_t *)malloc(total);
    if (!blob) {
        free(records);
        return FALSE;
    }
    memcpy(blob, &hdr, sizeof(hdr));
    if (count > 0)
        memcpy(blob + sizeof(hdr), records, body);
    free(records);

    BOOL ok = lass_store_put(QUEUE_STORE_DIR, QUEUE_STORE_NAME, blob, (uint32_t)total);
    free(blob);
    if (!ok)
        LOGW(LOG_MOD_QUEUE, "queue: could not write the queue to the store");
    return ok;
}

/* بلاب صف (قالب نسخه‌ی ۱ یا ۲) را parse می‌کند و دانلودها را به engine اضافه می‌کند */
static BOOL queue_load_buffer(IDM_Engine *eng, const uint8_t *data, size_t len, const char *source) {
    if (!data || len < sizeof(QueueFileHeader)) return FALSE;

    QueueFileHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != QUEUE_PERSIST_MAGIC ||
        (hdr.version != QUEUE_PERSIST_VERSION && hdr.version != QUEUE_PERSIST_VERSION_V1) ||
        hdr.count == 0 ||
        hdr.count > 10000) {  /* sanity check */
        return FALSE;
    }

    const uint8_t *body = data + sizeof(hdr);
    size_t avail = len - sizeof(hdr);

    /* نسخه‌ی ۱ رکورد بدون رمز دارد؛ همه به QueueRecordV2 تبدیل می‌شوند */
    QueueRecordV2 *records = (QueueRecordV2 *)calloc(hdr.count, sizeof(QueueRecordV2));
    if (!records) return FALSE;

    BOOL ok = FALSE;
    if (hdr.version == QUEUE_PERSIST_VERSION_V1) {
        size_t want = (size_t)hdr.count * sizeof(QueueRecord);
        QueueRecord *old = (avail >= want) ? (QueueRecord *)calloc(hdr.count, sizeof(QueueRecord)) : NULL;
        if (old) {
            memcpy(old, body, want);
            for (uint32_t k = 0; k < hdr.count; k++)
                records[k].base = old[k];
            ok = TRUE;
        }
        free(old);
    } else {
        size_t want = (size_t)hdr.count * sizeof(QueueRecordV2);
        if (avail >= want) {
            memcpy(records, body, want);
            ok = TRUE;
        }
    }
    if (!ok) {
        free(records);
        return FALSE;
    }

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        QueueRecordV2 *rv = &records[i];
        QueueRecord *r = &rv->base;

        /* فقط دانلودهایی رو بازیابی کن که منطقی هستن:
           - URL معتبر داشته باشن
           - فایل .tels یا فایل کامل روی دیسک باشه
           - در حالت completed/cancelled/failed/queued/paused باشن */
        if (!r->url[0] || !r->out_path[0]) continue;

        /* چک کن آیا فایل نهایی وجود داره */
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
        if (rv->pass_enc[0]) engine_dl_restore_password(dl, rv->pass_enc);

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
    LOGI(LOG_MOD_QUEUE, "Loaded %u/%u downloads from %s", loaded, hdr.count, source);
    return loaded > 0;
}

BOOL queue_persist_load(IDM_Engine *eng, const char *legacy_path) {
    if (!eng) return FALSE;

    uint8_t *blob = NULL;
    uint32_t blen = 0;
    if (lass_store_get(QUEUE_STORE_DIR, QUEUE_STORE_NAME, &blob, &blen)) {
        BOOL loaded = queue_load_buffer(eng, blob, blen, "store");
        free(blob);
        return loaded;
    }

    /* مهاجرت یک‌باره: فروشگاه صف ندارد و فایل قدیمی هست */
    if (!legacy_path || !legacy_path[0]) return FALSE;

    HANDLE hf = CreateFileA(legacy_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER fsz;
    if (!GetFileSizeEx(hf, &fsz) || fsz.QuadPart <= 0 || fsz.QuadPart > (LONGLONG)QUEUE_LEGACY_MAX) {
        CloseHandle(hf);
        return FALSE;
    }
    DWORD want = (DWORD)fsz.QuadPart;
    uint8_t *buf = (uint8_t *)malloc(want);
    DWORD got = 0;
    BOOL read_ok = buf && ReadFile(hf, buf, want, &got, NULL) && got == want;
    CloseHandle(hf);
    if (!read_ok) {
        free(buf);
        return FALSE;
    }

    BOOL loaded = queue_load_buffer(eng, buf, want, legacy_path);
    free(buf);
    if (loaded) {
        /* بلافاصله در فروشگاه ذخیره کن تا دفعه‌ی بعد از container خوانده شود */
        queue_persist_save(eng);
        LOGI(LOG_MOD_QUEUE, "queue migrated from %s into the store", legacy_path);
    }
    return loaded;
}
