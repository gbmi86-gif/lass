/* queue_persist.h — ذخیره و بارگذاری لیست دانلودها روی container .tels

   صف داخل فروشگاه برنامه (lass.tels، پوشه‌ی "queue") نگه داشته می‌شود.
   queue.dat قدیمی فقط برای مهاجرت یک‌باره‌ی صف خوانده می‌شود و دست‌نخورده می‌ماند.

   قالب بلاب صف (همان قالب فایل قدیمی):
     [MAGIC: 4 bytes "LASQ"] [VERSION: 2 bytes] [COUNT: 4 bytes] [RECORD: count × QueueRecordV2]
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include "../src/engine.h"

#define QUEUE_PERSIST_MAGIC      0x5141534Cu  /* "LASQ" */
#define QUEUE_PERSIST_VERSION_V1 1
#define QUEUE_PERSIST_VERSION    2   /* ۲: رکورد + رمز رمزشده‌ی FTP */

/* یه رکورد برای هر دانلود — اندازه‌ی ثابت برای سادگی */
typedef struct {
    uint32_t id;
    char     url[IDM_MAX_URL];
    char     out_path[IDM_MAX_PATH];
    char     filename[IDM_MAX_FILENAME];
    uint64_t total_size;
    uint64_t downloaded;
    uint32_t state;          /* DL_State */
    uint32_t num_segments;
    char     group_name[64];
    uint32_t group_id;
    BOOL     has_schedule;
    DWORD    sched_low;      /* FILETIME.dwLowDateTime */
    DWORD    sched_high;     /* FILETIME.dwHighDateTime */
} QueueRecord;

/* نسخه‌ی ۲: رکورد نسخه‌ی ۱ به‌علاوه‌ی رمز رمزشده با DPAPI (هرگز متن ساده نیست).
   بلاب‌های نسخه‌ی ۱ هم خوانده می‌شوند و رمزی ندارند. */
typedef struct {
    QueueRecord base;
    char        pass_enc[512];
} QueueRecordV2;

/* ذخیره‌ی کل صف در فروشگاه — بعد از هر تغییر (add/remove/complete) صدا زده می‌شود */
BOOL queue_persist_save(IDM_Engine *eng);

/* بارگذاری صف: اول از فروشگاه؛ اگر آنجا صفی نبود، یک‌بار از legacy_path مهاجرت می‌کند.
   legacy_path می‌تواند NULL باشد. */
BOOL queue_persist_load(IDM_Engine *eng, const char *legacy_path);

/* مسیر فایل قدیمی queue.dat: %APPDATA%\Lass\queue.dat — فقط برای مهاجرت */
void queue_persist_get_path(char *out, int out_sz);
