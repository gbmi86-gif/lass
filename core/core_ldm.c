/* core_ldm.c
   ─────────────────────────────────────────────────────────────
   Lass Download Manager – Core Implementation

   وظایف:
     - نگه‌داری engine و plugins
     - پردازش LDM_Event از GUO
     - تبدیل engine notifications به LDM_Notify برای GUO
     - ساخت LDM_Item snapshots از IDM_Download
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_ldm.h"
#include "../src/settings.h"    /* LassSettings */
#include "../src/engine.h"
#include "../src/segment.h"
#include "../src/types.h"
#include "../src/queue_persist.h"
#include "../plugins/plugins_setup.h"
#include "../plugins/plugin_clipboard.h"
#include "../plugins/plugin_basket.h"
#include "../plugins/plugin_proxy.h"
#include "../plugins/plugin_notify.h"
#include "../plugins/plugin_rtmp.h"
#include "../plugins/plugin_video_sniffer.h"
#include "../src/lass_log.h"

/* ======================================================
   ساختار داخلی
   ====================================================== */
struct LDM_Core {
    IDM_Engine  *engine;
    HWND         notify_hwnd;     /* WM_APP messages از engine */

    /* مقدار برگشتی plugins_init — قبلاً اصلاً ذخیره نمی‌شد و
       core_destroy به‌جاش plugins_shutdown(NULL) صدا می‌زد. چون
       plugins_shutdown اولین خطش `if (!reg) return;` هست، این یعنی
       plugin_registry_destroy و همه‌ی تک‌تک _plugin_destroy های
       پلاگین‌ها (dynamic_seg، speed_graph، browser_ext،
       speed_scheduler و بقیه) هیچ‌وقت واقعاً صدا زده نمی‌شدن — نه
       فقط دیر، بلکه هرگز. این دقیقاً یکی از دو علت اصلی timeout
       همیشگی shutdown_brake روی همین ۴ پلاگین (به‌علاوه
       engine_scheduler) بود که در لاگ واقعی دیده شد. */
    IDM_PluginRegistry *plugin_registry;

    LDM_NotifyCb notify_cb;
    void        *notify_userdata;

    /* window مخفی برای دریافت engine WM_APP messages */
    HWND         msg_hwnd;

    CRITICAL_SECTION lock;
};

/* پیام‌های داخلی از engine به core */
#define CORE_WM_ENGINE (WM_APP + 50)

/* forward */
static LRESULT CALLBACK core_msg_wnd_proc(HWND, UINT, WPARAM, LPARAM);
static void core_on_engine_msg(LDM_Core *core, UINT msg,
                                WPARAM wp, LPARAM lp);

/* ======================================================
   ساخت LDM_Item از IDM_Download
   caller باید eng->lock گرفته باشه
   ====================================================== */
static void fill_item(const IDM_Download *dl, LDM_Item *out) {
    memset(out, 0, sizeof(*out));
    out->id         = dl->id;
    out->state      = (int)dl->state;
    out->total_size = dl->total_size;
    out->downloaded = dl->downloaded;
    out->group_id   = dl->group_id;
    out->has_schedule = dl->has_schedule;
    if (dl->has_schedule) {
        SYSTEMTIME st;
        FILETIME ft_local;
        FileTimeToLocalFileTime(&dl->scheduled_at, &ft_local);
        FileTimeToSystemTime(&ft_local, &st);
        out->sched_hour   = st.wHour;
        out->sched_minute = st.wMinute;
    }

    snprintf(out->url,        sizeof(out->url),      "%s", dl->url);
    snprintf(out->filename,   sizeof(out->filename), "%s", dl->filename);
    snprintf(out->out_path,   sizeof(out->out_path), "%s", dl->out_path);
    snprintf(out->group_name, sizeof(out->group_name), "%s", dl->group_name);
    snprintf(out->fail_reason,sizeof(out->fail_reason), "%s", dl->fail_reason);

    /* سرعت */
    double spd = 0.0;
    if (dl->state == DL_DOWNLOADING && dl->segments) {
        for (uint32_t i = 0; i < dl->num_segments; i++)
            if (dl->segments[i]) spd += seg_get_speed(dl->segments[i]);
    }
    out->speed_bps = spd;

    /* درصد پیشرفت */
    if (dl->total_size > 0)
        out->progress_pct = (int)(dl->downloaded * 100LL / (int64_t)dl->total_size);
    else
        out->progress_pct = (dl->state == DL_COMPLETED) ? 100 : 0;
}

/* ======================================================
   پنجره message-only برای دریافت engine WM_APP
   ====================================================== */
#define CORE_WNDCLASS "LassCoreMsg"

static BOOL register_core_wndclass(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = CORE_WNDCLASS;
    wc.lpfnWndProc   = core_msg_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    return RegisterClassExA(&wc) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static LRESULT CALLBACK core_msg_wnd_proc(HWND hwnd, UINT msg,
                                            WPARAM wp, LPARAM lp) {
    LDM_Core *core = (LDM_Core *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    if (core && (msg == IDM_WM_PROGRESS || msg == IDM_WM_STATE ||
                 msg == IDM_WM_ADDED)) {
        core_on_engine_msg(core, msg, wp, lp);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ======================================================
   پردازش پیام‌های engine → LDM_Notify → GUO callback
   ====================================================== */
static void core_on_engine_msg(LDM_Core *core, UINT msg,
                                 WPARAM wp, LPARAM lp) {
    if (!core->notify_cb) return;

    uint32_t id = (uint32_t)wp;
    LDM_Notify n = {0};
    n.id = id;

    EnterCriticalSection(&core->engine->lock);
    IDM_Download *dl = engine_find(core->engine, id);

    if (msg == IDM_WM_ADDED) {
        n.type = LDM_NOTIFY_ADDED;
        if (dl) fill_item(dl, &n.item);

    } else if (msg == IDM_WM_PROGRESS) {
        n.type = LDM_NOTIFY_PROGRESS;
        if (dl) fill_item(dl, &n.item);

    } else if (msg == IDM_WM_STATE) {
        n.type = LDM_NOTIFY_STATE;
        if (dl) fill_item(dl, &n.item);
        else    n.item.state = (int)lp; /* دانلود حذف شده — state آخرش */

        /* ── fire هوک‌های چرخه‌عمر پلاگین ──
           این کاری بود که engine_bridge.c قرار بود انجام بده (با یه
           watch-thread جدا که هر ۵۰۰ms صف رو می‌گشت)، ولی
           bridge_engine_create هیچ‌وقت از هیچ‌جای برنامه‌ی واقعی صدا
           زده نمی‌شد — یعنی Dynamic Segment splitting و Smart Resume
           (نسخه‌ی پلاگینیش) عملاً هیچ‌وقت فعال نمی‌شدن. چون این‌جا
           (core_on_engine_msg) دقیقاً همون لحظه‌ای صدا زده میشه که
           engine.c واقعاً state رو عوض کرده، نیازی به یه watch-thread
           و ردیابی جداگانه‌ی "state قبلی" نیست — مستقیم روی state
           جدید سوییچ می‌کنیم. هر دو هوک (dynamic_seg، smart_resume)
           در برابر فراخوانی تکراری امن‌ان (خودشون چک idempotency
           دارن)، پس فرقی نمی‌کنه این start دقیقاً یه دانلود تازه
           باشه یا resume بعد از pause. */
        if (dl) {
            switch ((DL_State)lp) {
            case DL_DOWNLOADING:
                plugins_on_download_start(dl);
                break;
            case DL_COMPLETED:
                plugins_on_download_done(dl);
                break;
            case DL_PAUSED:
                plugins_on_download_pause(dl);
                break;
            case DL_FAILED:
            case DL_CANCELLED:
                plugins_on_download_fail(dl);
                break;
            default:
                break;
            }
        }

        /* notification وقتی دانلود تموم یا fail میشه */
        if ((DL_State)lp == DL_COMPLETED || (DL_State)lp == DL_FAILED) {
            IDM_PluginHooks *np = plugins_get_notify();
            if (np) {
                char fname[256] = {0};
                if (dl) strncpy(fname, dl->filename, sizeof(fname)-1);
                notify_on_download_complete(np, fname,
                    (DL_State)lp == DL_COMPLETED);

                /* هشدار «اسکن نشده» — فقط برای فایل‌های اجرایی/نصب‌کننده
                   و فقط وقتی دانلود واقعاً موفق بوده. عمداً یه لیست
                   ثابت و مستقل از categories.ini استفاده می‌کنیم (نه
                   دسته‌ی قابل‌ویرایش «Software» کاربر)، چون این یه
                   ملاحظه‌ی امنیتیه، نه دسته‌بندی فایل — نباید با تغییر
                   دادن/حذف کردن یه دسته توسط کاربر از کار بیفته. */
                if ((DL_State)lp == DL_COMPLETED && fname[0]) {
                    static const char *exec_exts[] = {
                        ".exe", ".msi", ".msix", ".scr", ".bat", ".cmd",
                        ".ps1", ".apk", ".jar", NULL
                    };
                    const char *dot = strrchr(fname, '.');
                    if (dot) {
                        for (int i = 0; exec_exts[i]; i++) {
                            if (_stricmp(dot, exec_exts[i]) == 0) {
                                notify_send(np, NOTIFY_UNSCANNED, fname, NULL);
                                break;
                            }
                        }
                    }
                }
            }
            /* queue رو save کن تا state جدید (completed/failed) ذخیره بشه */
            LeaveCriticalSection(&core->engine->lock);
            char qp[MAX_PATH];
            queue_persist_get_path(qp, sizeof(qp));
            queue_persist_save(core->engine, qp);
            EnterCriticalSection(&core->engine->lock);
        }
    }

    LeaveCriticalSection(&core->engine->lock);

    core->notify_cb(&n, core->notify_userdata);
}

/* ======================================================
   Clipboard + Basket callbacks — از GUO ثبت میشن
   ولی core آنها رو به GUO forward می‌کنه
   ====================================================== */
static LDM_Core *g_core_inst = NULL; /* singleton برای callbacks */

static void core_on_clipboard_url(const char *url, void *ud) {
    (void)ud;
    if (!g_core_inst || !g_core_inst->notify_cb || !url) return;

    /* اگه لینک یه صفحه‌ی ویدیویی شناخته‌شده باشه (نه لینک مستقیم
       فایل)، از طریق sniffer ردش کن تا کیفیت‌ها پیدا بشن، به جای
       اینکه به عنوان یه دانلود عادی (که اشتباه خواهد بود) دیده بشه */
    VS_Source vsrc = vs_detect_source(url);
    if (vsrc == VS_SRC_YOUTUBE || vsrc == VS_SRC_APARAT ||
        vsrc == VS_SRC_INSTAGRAM || vsrc == VS_SRC_TWITTER ||
        vsrc == VS_SRC_VIMEO) {
        IDM_PluginHooks *vs = plugins_get_video_sniffer();
        if (vs) { vs_sniff_async(vs, url, NULL, NULL); return; }
    }

    LDM_Notify n = {0};
    n.type = LDM_NOTIFY_CLIP_URL;
    snprintf(n.url, sizeof(n.url), "%s", url);
    g_core_inst->notify_cb(&n, g_core_inst->notify_userdata);
}

static void core_on_basket_drop(const char *url, void *ud) {
    core_on_clipboard_url(url, ud);
}

static void core_on_ext_download(const char *url, const char *fname,
                                  const char *ref, void *ud) {
    (void)ud;
    (void)fname;

    /* اگه URL از سایت‌های ویدیویی شناخته‌شده باشه (یوتیوب، آپارات،
       اینستاگرام، توییتر و...) از طریق video sniffer ردش کن تا
       کاربر بتونه کیفیت‌های مختلف رو انتخاب کنه — دقیقاً شبیه
       رفتار "Video Sniffer" در IDM */
    if (vs_is_video_url(url)) {
        IDM_PluginHooks *vs = plugins_get_video_sniffer();
        if (vs) {
            vs_sniff_async(vs, url, ref, fname);
            return;
        }
    }

    /* در غیر این صورت: تایید دستی از کاربر — ولی برخلاف قبل، این‌جا
       مستقیم core_on_clipboard_url رو صدا نمی‌زنیم، چون اون تابع
       Referer رو دور می‌ریزه و نوع رویداد رو هم اشتباه CLIP_URL
       می‌ذاره (نه EXT_URL). خیلی از سایت‌های میزبان فایل (مخصوصاً
       mirrorهای دانلود گیم/رپک) بدون Referer معتبر با ۴۰۳ رد
       می‌کنن — این‌جا دقیقاً همون‌جاییه که قبلاً Referer گم می‌شد. */
    if (!g_core_inst || !g_core_inst->notify_cb || !url) return;

    LDM_Notify n = {0};
    n.type = LDM_NOTIFY_EXT_URL;
    snprintf(n.url, sizeof(n.url), "%s", url);
    if (ref && ref[0])
        snprintf(n.referer, sizeof(n.referer), "%s", ref);
    g_core_inst->notify_cb(&n, g_core_inst->notify_userdata);
}

/* ======================================================
   Public API
   ====================================================== */
LDM_Core *core_create(HWND notify_hwnd) {
    LDM_Core *core = (LDM_Core *)calloc(1, sizeof(*core));
    if (!core) return NULL;

    InitializeCriticalSection(&core->lock);
    g_core_inst = core;

    /* پنجره message-only */
    register_core_wndclass();
    core->msg_hwnd = CreateWindowExA(0, CORE_WNDCLASS, NULL,
                                      0, 0, 0, 0, 0,
                                      HWND_MESSAGE, NULL,
                                      GetModuleHandleA(NULL), NULL);
    SetWindowLongPtrA(core->msg_hwnd, GWLP_USERDATA, (LONG_PTR)core);

    /* engine با msg_hwnd — تمام WM_APP به core میاد */
    core->notify_hwnd = notify_hwnd;
    core->engine = engine_create(core->msg_hwnd);
    if (!core->engine) {
        DestroyWindow(core->msg_hwnd);
        DeleteCriticalSection(&core->lock);
        free(core);
        return NULL;
    }

    /* راه‌اندازی plugins */
    plugins_set_speed_graph_parent(notify_hwnd, core->engine);
    plugins_set_speed_scheduler_engine(core->engine);
    plugins_set_basket_params(notify_hwnd, core->engine,
                               core_on_basket_drop, NULL);
    plugins_set_clipboard_params(notify_hwnd,
                                  core_on_clipboard_url, NULL);
    plugins_set_browser_ext_cb(core_on_ext_download, NULL);

    /* notify: tray hwnd بعداً از GUO set میشه */
    plugins_set_notify_params(notify_hwnd, 9001);

    /* ini path برای categories */
    char ini[MAX_PATH];
    settings_get_ini_path(ini, sizeof(ini));
    plugins_set_ini_path(ini);

    /* قبلاً مقدار برگشتی اینجا اصلاً ذخیره نمی‌شد و core_destroy
       مجبور بود NULL به plugins_shutdown بده — که به‌خاطر گارد
       `if (!reg) return;` داخل خودش، عملاً کل تابع رو no-op
       می‌کرد. حالا واقعاً نگه‌ش می‌داریم. */
    core->plugin_registry = plugins_init();

    /* بارگذاری queue از آخرین session — اگه queue.dat وجود داشت
       و دانلودهای قبلی داشت، به لیست اضافه میشن. این باعث میشه
       بعد از کرش، قطع برق، یا بستن برنامه، لیست دانلودها حفظ بشه.
       دانلودهای ناقص با state DL_PAUSED بارگذاری میشن (نه auto-start)
       تا کاربر خودش تصمیم بگیره resume کنه. */
    {
        char qp[MAX_PATH];
        queue_persist_get_path(qp, sizeof(qp));
        queue_persist_load(core->engine, qp);
    }

    LOGI(LOG_MOD_CORE, "created (engine=%p)", (void*)core->engine);
    return core;
}

void core_signal_shutdown(LDM_Core *core) {
    if (!core) return;
    /* باید *قبل* از shutdown_brake_wait_all در main.c صدا زده
       بشه — رجوع کن به توضیح کامل در plugins_setup.h/main.c.
       فقط پرچم‌ها رو ست می‌کنه، هیچ wait/join/free‌ای اینجا نیست. */
    engine_signal_shutdown(core->engine);
    plugins_signal_shutdown(core->plugin_registry);
}

void core_destroy(LDM_Core *core) {
    if (!core) return;

    plugins_shutdown(core->plugin_registry);
    engine_destroy(core->engine);

    if (core->msg_hwnd) DestroyWindow(core->msg_hwnd);
    DeleteCriticalSection(&core->lock);
    g_core_inst = NULL;
    free(core);
    LOGI(LOG_MOD_CORE, "destroyed");
}

void core_set_notify_cb(LDM_Core *core, LDM_NotifyCb cb, void *userdata) {
    if (!core) return;
    core->notify_cb       = cb;
    core->notify_userdata = userdata;
}

/* ======================================================
   Event dispatch
   ====================================================== */
void core_dispatch(LDM_Core *core, const LDM_Event *evt) {
    if (!core || !evt) return;

    switch (evt->type) {
    case LDM_EVT_ADD: {
        const char *url = evt->data.add.url;

        /* ── RTMP: مستقیم به plugin بفرست ── */
        if (rtmp_is_rtmp_url(url)) {
            IDM_PluginHooks *rp = plugins_get_rtmp();
            if (rp) {
                rtmp_download(rp, url, evt->data.add.out_path,
                              core->notify_hwnd);
            } else {
                LOGI(LOG_MOD_CORE, "RTMP plugin not available");
            }
            break;
        }

        /* HTTP/HTTPS/FTP: به engine */
        IDM_Download *added = engine_add(core->engine, url,
                   evt->data.add.out_path,
                   evt->data.add.segments ? evt->data.add.segments : 8,
                   evt->data.add.speed_limit);
        if (added && evt->data.add.referer[0])
            engine_set_referer(added, evt->data.add.referer);
        /* ذخیره queue بعد از هر اضافه شدن */
        { char qp[MAX_PATH]; queue_persist_get_path(qp, sizeof(qp));
          queue_persist_save(core->engine, qp); }
        break;
    }

    case LDM_EVT_ADD_BATCH: {
        const LDM_BatchParams *b = &evt->data.batch;
        engine_add_batch(core->engine, b->urls_newline,
                         b->out_dir, b->segments, 0, b->group_name);
        free(b->urls_newline); /* core مسئول free هستش */
        { char qp[MAX_PATH]; queue_persist_get_path(qp, sizeof(qp));
          queue_persist_save(core->engine, qp); }
        break;
    }

    case LDM_EVT_PAUSE:
        engine_pause(core->engine, evt->data.id);
        { char qp[MAX_PATH]; queue_persist_get_path(qp, sizeof(qp));
          queue_persist_save(core->engine, qp); }
        break;

    case LDM_EVT_RESUME:
        engine_resume(core->engine, evt->data.id);
        break;

    case LDM_EVT_CANCEL:
        engine_cancel(core->engine, evt->data.id);
        { char qp[MAX_PATH]; queue_persist_get_path(qp, sizeof(qp));
          queue_persist_save(core->engine, qp); }
        break;

    case LDM_EVT_REMOVE: {
        engine_remove(core->engine, evt->data.id);
        LDM_Notify n = {.type=LDM_NOTIFY_REMOVED, .id=evt->data.id};
        if (core->notify_cb) core->notify_cb(&n, core->notify_userdata);
        { char qp[MAX_PATH]; queue_persist_get_path(qp, sizeof(qp));
          queue_persist_save(core->engine, qp); }
        break;
    }

    case LDM_EVT_CLEAR_DONE: {
        int n = engine_clear_completed(core->engine);
        if (core->notify_cb) {
            LDM_Notify notify = {.type=LDM_NOTIFY_CLEARED, .count=n};
            core->notify_cb(&notify, core->notify_userdata);
        }
        { char qp[MAX_PATH]; queue_persist_get_path(qp, sizeof(qp));
          queue_persist_save(core->engine, qp); }
        break;
    }

    case LDM_EVT_SCHEDULE:
        engine_schedule(core->engine,
                        evt->data.schedule.id,
                        evt->data.schedule.hour,
                        evt->data.schedule.minute);
        break;

    case LDM_EVT_QUIT:
        PostQuitMessage(0);
        break;

    default: break;
    }
}

/* ── shorthand helpers ── */
void core_add(LDM_Core *core, const char *url, const char *path,
              uint32_t segs, uint64_t speed_limit) {
    core_add_ex(core, url, path, segs, speed_limit, NULL);
}

void core_add_ex(LDM_Core *core, const char *url, const char *path,
                  uint32_t segs, uint64_t speed_limit,
                  const char *referer) {
    LDM_Event e = {.type = LDM_EVT_ADD};
    snprintf(e.data.add.url,      sizeof(e.data.add.url),      "%s", url);
    snprintf(e.data.add.out_path, sizeof(e.data.add.out_path), "%s", path);
    e.data.add.segments    = segs;
    e.data.add.speed_limit = speed_limit;
    if (referer && referer[0])
        snprintf(e.data.add.referer, sizeof(e.data.add.referer), "%s", referer);
    core_dispatch(core, &e);
}

void core_batch(LDM_Core *core, const char *urls,
                const char *dir, const char *group) {
    if (!urls) return;
    LDM_Event e = {.type = LDM_EVT_ADD_BATCH};
    e.data.batch.urls_newline = _strdup(urls); /* core free می‌کنه */
    snprintf(e.data.batch.out_dir,    sizeof(e.data.batch.out_dir),    "%s", dir   ? dir   : "");
    snprintf(e.data.batch.group_name, sizeof(e.data.batch.group_name), "%s", group ? group : "");
    e.data.batch.segments = 8;
    core_dispatch(core, &e);
}

/* مشابه core_batch ولی synchronous و تعداد دانلودهای واقعاً اضافه‌شده
   رو برمی‌گردونه — برای فیدبک درست به کاربر در Batch dialog. چون
   core_dispatch خودش synchronous عمل می‌کنه (نه async از طریق
   thread/queue جدا)، فراخوانی مستقیم engine_add_batch اینجا امنه. */
int core_batch_count(LDM_Core *core, const char *urls,
                     const char *dir, const char *group) {
    if (!core || !urls) return 0;
    return engine_add_batch(core->engine, urls, dir, 8, 0, group);
}

void core_pause  (LDM_Core *c, uint32_t id) { LDM_Event e={LDM_EVT_PAUSE,   {.id=id}}; core_dispatch(c,&e); }
void core_resume (LDM_Core *c, uint32_t id) { LDM_Event e={LDM_EVT_RESUME,  {.id=id}}; core_dispatch(c,&e); }
void core_cancel (LDM_Core *c, uint32_t id) { LDM_Event e={LDM_EVT_CANCEL,  {.id=id}}; core_dispatch(c,&e); }
void core_remove (LDM_Core *c, uint32_t id) { LDM_Event e={LDM_EVT_REMOVE,  {.id=id}}; core_dispatch(c,&e); }

int core_clear_done(LDM_Core *c) {
    LDM_Event e={.type=LDM_EVT_CLEAR_DONE};
    core_dispatch(c,&e);
    return 0;
}

void core_schedule(LDM_Core *c, uint32_t id, int h, int m) {
    LDM_Event e={.type=LDM_EVT_SCHEDULE,
                 .data={.schedule={.id=id,.hour=h,.minute=m}}};
    core_dispatch(c,&e);
}

/* ── state queries ── */
LDM_Stats core_get_stats(LDM_Core *core) {
    LDM_Stats s = {0};
    if (!core) return s;

    EnterCriticalSection(&core->engine->lock);
    for (IDM_Download *dl = core->engine->queue_head; dl; dl = dl->next) {
        s.total_count++;
        if (dl->state == DL_DOWNLOADING || dl->state == DL_LAUNCHING)
            s.active_count++;
        else if (dl->state == DL_QUEUED)
            s.queued_count++;
    }
    LeaveCriticalSection(&core->engine->lock);
    s.total_speed_bps = engine_total_speed(core->engine);
    return s;
}

bool core_get_item(LDM_Core *core, uint32_t id, LDM_Item *out) {
    if (!core || !out) return false;
    EnterCriticalSection(&core->engine->lock);
    IDM_Download *dl = engine_find(core->engine, id);
    if (dl) fill_item(dl, out);
    LeaveCriticalSection(&core->engine->lock);
    return dl != NULL;
}

int core_list_items(LDM_Core *core, LDM_Item *out, int max) {
    if (!core || !out || max <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&core->engine->lock);
    for (IDM_Download *dl = core->engine->queue_head;
         dl && n < max; dl = dl->next) {
        fill_item(dl, &out[n++]);
    }
    LeaveCriticalSection(&core->engine->lock);
    return n;
}

void *core_get_engine(LDM_Core *core) {
    return core ? core->engine : NULL;
}

void core_settings_apply(LDM_Core *core, const LassSettings *s) {
    if (!core || !s) return;
    core->engine->max_concurrent = s->max_concurrent;

    IDM_PluginHooks *cb = plugins_get_clipboard();
    if (cb) clipboard_plugin_set_enabled(cb, s->clipboard_monitor);

    IDM_PluginHooks *bk = plugins_get_basket();
    if (bk) basket_show(bk, s->basket_visible);

    core_set_proxy(core, s->proxy_url,
                   s->proxy_enabled && s->proxy_url[0]);
}

void core_set_proxy(LDM_Core *core, const char *url, bool enabled) {
    (void)core;
    IDM_PluginHooks *pr = plugins_get_proxy();
    if (!pr) return;
    if (enabled && url && url[0])
        proxy_plugin_set_from_url(pr, url);
    else
        proxy_plugin_clear(pr);
}
