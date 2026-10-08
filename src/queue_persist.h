/* queue_persist.h — ذخیره و بارگذاری لیست دانلودها
   هدف: وقتی برنامه بسته یا کرش می‌کنه، بعد از restart
   لیست دانلودها و وضعیتشون بازیابی بشه.

   فرمت فایل queue.dat:
     [MAGIC: 4 bytes "LASQ"]
     [VERSION: 2 bytes]
     [COUNT: 4 bytes]
     [RECORD: count × sizeof(QueueRecord)]
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include "../src/engine.h"

#define QUEUE_PERSIST_MAGIC   0x5141534Cu  /* "LASQ" */
#define QUEUE_PERSIST_VERSION 1

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

/* ذخیره کل queue به فایل — بعد از هر تغییر (add/remove/complete) صدا زده میشه */
BOOL queue_persist_save(IDM_Engine *eng, const char *queue_path);

/* بارگذاری queue از فایل — در startup صدا زده میشه */
BOOL queue_persist_load(IDM_Engine *eng, const char *queue_path);

/* مسیر پیش‌فرض: %APPDATA%\Lass\queue.dat */
void queue_persist_get_path(char *out, int out_sz);
