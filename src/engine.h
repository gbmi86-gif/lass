/* engine.h  –  core download engine */
#ifndef IDM_ENGINE_H
#define IDM_ENGINE_H

#include "types.h"
#include "segment.h"
#include "storage.h"
#include "resume.h"

/* حداکثر تعداد retry خودکار برای شکست discovery/launch — بعد
   از این تعداد، دانلود به DL_FAILED دائمی میره و نیاز به دخالت
   دستی کاربر داره */
#define ENGINE_MAX_LAUNCH_RETRY 5

/* حداقل فاصله‌ی زمانی بین شروع discovery دو دانلود مختلف که به
   یه host واحد میرن — جلوگیری از پر شدن ناگهانی صف WinINet */
#define ENGINE_HOST_STAGGER_MS 1500
#define ENGINE_HOST_STAGGER_SLOTS 16

/* ======================================================
   IDM_Download  –  one download job
   ====================================================== */
struct IDM_Download {
    uint32_t         id;
    DL_State         state;

    /* اشاره‌گر به engine مادر — فقط یه بار در engine_add ست میشه و
       بعدش هرگز عوض نمیشه، پس خوندنش بدون lock امنه. هدف: به
       پلاگین‌هایی مثل dynamic_seg اجازه بده قبل از دست‌کاری
       segments/num_segments این دانلود، همون eng->lock ای که خود
       engine.c برای این آرایه استفاده می‌کنه رو بگیرن — بدون این،
       یه realloc همزمان (از plugin) با یه پیمایش همزمان (از
       engine.c، مثلاً engine_pause) دقیقاً یه use-after-free/heap
       corruption واقعیه که با افزایش تعداد دانلود/segment همزمان
       بیشتر رخ میده. */
    IDM_Engine      *eng;

    char             url[IDM_MAX_URL];
    /* ── Referer ──
       بعضی سایت‌های میزبان فایل (خصوصاً mirrorهای دانلود گیم/رپک،
       مثل همینی که در گزارش واقعی کاربر با HTTP 403 رد شد) درخواست
       بدون Referer معتبر رو hotlink-protection حساب می‌کنن و رد
       می‌کنن — حتی اگه هیچ ربطی به تعداد کانکشن همزمان نداشته باشه.
       این فیلد از طریق افزونه‌ی مرورگر (که صفحه‌ی مبدأ رو می‌دونه)
       پر میشه؛ خالی = هیچ Referer header‌ای فرستاده نمیشه. */
    char             referer[IDM_MAX_URL];
    char             out_path[IDM_MAX_PATH];  /* مسیر نهایی واقعی فایل */
    char             temp_path[IDM_MAX_PATH]; /* مسیر فایل .tels حین دانلود —
                                                  بعد از تکمیل به out_path
                                                  rename می‌شه */
    char             filename[IDM_MAX_FILENAME];

    /* دلیل خوانا برای Failed — کاربر باید بفهمه چرا، نه فقط
       اینکه "Failed" شده (مثلاً "Link not found (404)" در برابر
       "Network error, retrying" مبهم) */
    char             fail_reason[96];

    uint64_t         total_size;
    volatile LONGLONG downloaded;    /* atomic via InterlockedAdd64 */
    uint64_t         speed_limit;    /* bytes/sec, 0 = unlimited */

    uint32_t         num_segments;
    IDM_Segment    **segments;       /* array of pointers */

    IDM_FileWriter  *writer;

    /* Resume data (loaded from state file) */
    IDM_ResumeSeg   *resume_segs;    /* NULL if fresh download */

    /* Config snapshot */
    uint32_t         max_segments;
    bool             accept_ranges;
    /* true وقتی این دانلود با مسیر HLS (کل جریان در launch thread) اجرا می‌شود.
       مانیتور چنین دانلودی را خودش تکمیل نمی‌کند. */
    bool             hls_mode;
    /* تملک برای کار بیرون از lock (pause/cancel/launch/مانیتور): busy>0 یعنی یک thread
       همین الان به segment ها یا thread launch وابسته است؛ remove_pending یعنی حذف
       خواسته شده ولی تخریب به آخرین صاحب busy سپرده شده. */
    uint32_t         busy;
    bool             remove_pending;
    /* رمز FTP: فقط در حافظه (secret_url) و به‌صورت رمزشده با DPAPI (pass_enc).
       هرگز به‌صورت متن ساده در صف یا لاگ نمی‌آید. */
    char             secret_url[IDM_MAX_URL];
    char             pass_enc[512];

    /* validator های فایل روی سرور (از HEAD اولیه) — برای If-Range
       موقع ادامه‌ی دانلود: به‌جای اینکه کورکورانه فرض کنیم فایل
       همونه، از خود سرور می‌پرسیم. خالی = سرور نفرستاده. */
    char             etag[128];
    char             last_modified[64];

    /* Callbacks */
    IDM_ProgressCb   on_progress;
    IDM_StateCb      on_state;
    void            *userdata;

    /* Linked list for queue */
    IDM_Download    *next;

    /* ── زمان‌بندی ── */
    FILETIME         scheduled_at;  /* زمان شروع — {0,0} = فوری */
    BOOL             has_schedule;

    /* ── صف گروهی ── */
    uint32_t         group_id;      /* 0 = بدون گروه */
    char             group_name[64];

    /* ── retry خودکار برای شکست discovery/launch (نه segment) ──
       وقتی http_discover fail بشه (مثلاً سرور موقتاً connection
       رو رد کنه چون تعداد connection همزمان از این کلاینت زیاده
       — یه محدودیت رایج در سایت‌های دانلود)، به‌جای رفتن مستقیم
       به DL_FAILED دائمی، چند بار با backoff افزایشی retry
       می‌کنیم. این باعث میشه دانلودهای همزمان از یه سرور واحد
       خودشون رو بدون دخالت دستی کاربر "heal" کنن. */
    uint32_t         launch_retry_count;
    DWORD             next_launch_retry_at; /* GetTickCount هدف */

    /* Sync */
    CRITICAL_SECTION lock;
};

/* ======================================================
   IDM_Engine  –  manages a queue of downloads
   ====================================================== */
struct IDM_Engine {
    IDM_Download    *queue_head;   /* اول صف (قدیمی‌ترین) */
    IDM_Download    *queue_tail;   /* آخر صف (جدیدترین) */
    uint32_t         next_id;
    uint32_t         max_concurrent;
    uint32_t         active_count;
    volatile LONG    refs;          /* refcount: صاحب + scheduler + هر launch_thread زنده */

    HANDLE           scheduler;     /* background thread */
    HANDLE           wake_event;    /* SetEvent to wake scheduler */
    volatile bool    shutdown;

    CRITICAL_SECTION lock;

    /* GUI notification window (posts WM_APP messages) */
    HWND             notify_hwnd;

    /* ── stagger کردن discovery برای دانلودهای همزمان از یه host ──
       مشکل: وقتی چند دانلود مختلف (نه segment های یه دانلود) به
       یه سرور واحد میرن، اگه همه در یه scheduler tick شروع به
       discovery کنن، WinINet queue داخلی خودش رو برای اون سرور پر
       می‌کنه و بقیه با timeout (err=12002) fail میشن — حتی اگه
       مشکل شبکه‌ای واقعی نباشه. راه‌حل: قبل از dispatch یه دانلود
       جدید، چک کن آخرین بار کِی یه دانلود دیگه به همون host
       dispatch شده؛ اگه اخیراً بوده، صبر کن. این یه جدول کوچیک و
       ثابت‌اندازه‌ست، بدون malloc، هزینه‌ی محاسباتی ناچیز. */
    char             host_stagger_name[ENGINE_HOST_STAGGER_SLOTS][128];
    DWORD            host_stagger_at[ENGINE_HOST_STAGGER_SLOTS];
};

/* ======================================================
   Engine API
   ====================================================== */
IDM_Engine  *engine_create(HWND notify_hwnd);
void         engine_destroy(IDM_Engine *eng);

/* فقط پرچم shutdown رو ست و scheduler thread رو بیدار می‌کنه —
   بدون هیچ صبر/join/آزادسازی‌ای. هدف: قابل صدا زدن باشه *قبل* از
   shutdown_brake_wait_all در main.c، تا وقتی برنامه منتظر خاموش
   شدن engine_scheduler می‌مونه، اون thread واقعاً خبردار شده باشه
   که باید خارج بشه (رجوع کن به توضیح کامل در main.c و
   plugins_signal_shutdown). idempotent — صدا زدن چندباره (یا صدا
   نزدن، چون engine_destroy هم خودش این کار رو می‌کنه) کاملاً امنه. */
void         engine_signal_shutdown(IDM_Engine *eng);

/* دسترسی امن به یک دانلود از بیرون engine: اگر دانلود با این id هنوز در لیست باشد،
   busy را بالا می‌برد و اشاره‌گر را برمی‌گرداند؛ وگرنه NULL. هر acquire موفق باید
   با engine_dl_release جفت شود. */
IDM_Download *engine_dl_acquire(IDM_Engine *eng, uint32_t id);
/* busy را پایین می‌آورد؛ اگر حذف معلق باشد و صاحب دیگری نماند، تخریب را انجام می‌دهد.
   بعد از این فراخوانی dl ممکن است دیگر معتبر نباشد. */
void          engine_dl_release(IDM_Engine *eng, IDM_Download *dl);
/* رمز رمزشده‌ی صف را برمی‌گرداند و secret_url را برای اتصال می‌سازد */
BOOL engine_dl_restore_password(IDM_Download *dl, const char *pass_enc);

/* گزارش یه stall واقعی (بعد از عبور از stall_timeout_sec، نه یه
   خوندن لحظه‌ای) روی یه host مشخص — dynamic_seg این رو موقع
   restart واقعی یه segment stall‌شده صدا می‌زنه. pick_seg_count
   (در engine.c) از این برای شروع محتاطانه‌تر روی سرورهایی که اخیراً
   چند-اتصالی رو دوست نداشتن استفاده می‌کنه. host باید فقط hostname
   باشه (بدون scheme/path) — دقیقاً چیزی که http_extract_host
   برمی‌گردونه. */
void         engine_report_host_trouble(const char *host);

/* استخراج URL های http/https/ftp از متن آزاد، یکتا، هر کدوم در یک
   خط ('\n')، بایت‌های غیر-ASCII به %XX. out==NULL: فقط می‌شمره.
   بافر امن: strlen(text)*3+2 بایت. برمی‌گردونه: تعداد URL یکتا. */
int          engine_extract_urls(const char *text, char *out, size_t out_sz);

/* فقط برای تست — wrapper صادرشده‌ی pick_seg_count (که static
   هست) تا test_all.c بتونه منطق واقعی «شروع محتاطانه‌تر روی
   host پرمشکل» رو مستقیم تأیید کنه. */
uint32_t     engine_test_pick_seg_count(const char *host, uint64_t size,
                                        uint32_t max_seg);

IDM_Download *engine_add(IDM_Engine *eng,
                         const char *url,
                         const char *out_path,
                         uint32_t    max_segments,
                         uint64_t    speed_limit);

/* Referer اختیاری بعد از engine_add ست میشه (مثلاً از افزونه‌ی
   مرورگر که می‌دونه دانلود از کدوم صفحه شروع شده) — برای سایت‌هایی
   که بدون Referer معتبر با 403 رد می‌کنن. هر کاراکتر CR/LF برای
   جلوگیری از HTTP header injection حذف میشه. */
void engine_set_referer(IDM_Download *dl, const char *referer);

void engine_pause (IDM_Engine *eng, uint32_t id);
void engine_resume(IDM_Engine *eng, uint32_t id);

/* محدودیت سرعت سراسری — برای استفاده‌ی پلاگین Speed Scheduler.
   thread-safe بدون نیاز به lock چون فقط یه عدد ساده هست. */
void     engine_set_global_speed_limit(IDM_Engine *eng, uint64_t bytes_per_sec);
uint64_t engine_get_global_speed_limit(IDM_Engine *eng);
void engine_cancel(IDM_Engine *eng, uint32_t id);
void engine_remove(IDM_Engine *eng, uint32_t id);

/* ── زمان‌بند ── */
void engine_schedule  (IDM_Engine *eng, uint32_t id, int hour, int minute);
void engine_unschedule(IDM_Engine *eng, uint32_t id);

/* ── صف گروهی ── */
int engine_add_batch(IDM_Engine *eng,
                     const char *urls_newline,
                     const char *out_dir,
                     uint32_t    max_segments,
                     uint64_t    speed_limit,
                     const char *group_name);

/* ── حذف همه completed از صف ── */
int engine_clear_completed(IDM_Engine *eng);

IDM_Download *engine_find(IDM_Engine *eng, uint32_t id);

/* Aggregate speed across all active downloads */
double engine_total_speed(IDM_Engine *eng);

/* ======================================================
   WM_APP sub-messages sent to notify_hwnd
   ====================================================== */
#define IDM_WM_PROGRESS   (WM_APP + 1)   /* wParam=id, lParam=0 */
#define IDM_WM_STATE      (WM_APP + 2)   /* wParam=id, lParam=new_state */
#define IDM_WM_ADDED      (WM_APP + 3)   /* wParam=id */

#endif /* IDM_ENGINE_H */
