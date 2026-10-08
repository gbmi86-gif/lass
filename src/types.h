#ifndef IDM_TYPES_H
#define IDM_TYPES_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ======================================================
   IDM-Clone  –  shared type definitions
   ====================================================== */

#define IDM_VERSION_STR   "1.0.0"
#define IDM_MAX_SEGMENTS  16
#define IDM_MAX_URL       2048
#define IDM_MAX_PATH      MAX_PATH
#define IDM_MAX_FILENAME  256
#define IDM_STATE_FILE_EXT ".idmstate"

/* ---------- result codes ---------- */
typedef enum {
    IDM_OK               =  0,
    IDM_ERR_NOMEM        = -1,
    IDM_ERR_INVALID      = -2,
    IDM_ERR_NETWORK      = -3,
    IDM_ERR_IO           = -4,
    IDM_ERR_CHECKSUM     = -5,
    IDM_ERR_CANCELLED    = -6,
    IDM_ERR_MAX_RETRY    = -7,
    IDM_ERR_CORRUPT      = -8,
    IDM_ERR_NOT_FOUND    = -9,
    IDM_ERR_NO_RANGE     = -10
} IDM_Result;

/* ---------- download states ---------- */
typedef enum {
    DL_IDLE        = 0,
    DL_QUEUED,
    DL_CONNECTING,
    DL_DOWNLOADING,
    /* حالت گذرا: Pause زده شده، دانلود دیگه active نیست (از
       eng->active_count کم شده) ولی segment thread ها هنوز دارن
       واقعاً متوقف می‌شن (می‌تونه تا 10 ثانیه به ازای هر segment
       طول بکشه). تا وقتی این حالته، resume/cancel/remove نباید
       dl->segments رو دست بزنن — چون engine_pause خودش هنوز داره
       همون آرایه رو می‌خونه/دستکاری می‌کنه. رفع یه race واقعی که
       باعث یه crash واقعی (null-deref در resume_save) شده بود:
       قبلاً engine_pause فوراً DL_PAUSED ست می‌کرد و درنتیجه
       کاربر می‌تونست همون لحظه Resume بزنه درحالی‌که pause هنوز
       داشت segment های گیرکرده رو (zombie) پاک‌سازی می‌کرد —
       engine_resume هم زودتر از موعد dl->segments رو NULL می‌کرد
       درحالی‌که engine_pause هنوز داشت با همون آرایه کار می‌کرد. */
    DL_PAUSING,
    DL_PAUSED,
    DL_VERIFYING,
    DL_COMPLETED,
    DL_FAILED,
    DL_CANCELLED,
    DL_LAUNCHING    /* داخلی: در حال HTTP discover در thread مجزا */
} DL_State;

/* ---------- segment states ---------- */
typedef enum {
    SEG_IDLE = 0,
    SEG_CONNECTING,
    SEG_DOWNLOADING,
    SEG_PAUSED,
    SEG_COMPLETED,
    SEG_FAILED,
    SEG_RETRYING
} SEG_State;

/* ---------- forward declarations ---------- */
typedef struct IDM_Download  IDM_Download;
typedef struct IDM_Segment   IDM_Segment;
typedef struct IDM_Engine    IDM_Engine;
typedef struct IDM_Queue     IDM_Queue;

/* ---------- callbacks ---------- */
typedef void (*IDM_ProgressCb)(IDM_Download *dl, void *userdata);
typedef void (*IDM_StateCb)   (IDM_Download *dl, DL_State old_st, DL_State new_st, void *userdata);

#endif /* IDM_TYPES_H */
