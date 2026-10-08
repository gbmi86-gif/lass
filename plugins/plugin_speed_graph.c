/* plugin_speed_graph.c
   ─────────────────────────────────────────────────────────────
   Speed Graph Plugin – نمودار سرعت دانلود با GDI

   معماری:
     - یه ring buffer برای SG_HISTORY_SECONDS نمونه سرعت
     - یه timer thread که هر ثانیه سرعت کل رو ثبت می‌کنه
     - یه پنجره GDI که نمودار رو رسم می‌کنه
     - کاملاً از طریق IDM_PluginHooks وصل میشه

   thread safety:
     - g_sg_lock برای ring buffer
     - WM_PAINT در thread GUI اجرا میشه (safe)
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "plugin_speed_graph.h"
#include "../src/shutdown_brake.h"
#include "../src/lass_log.h"

/* ======================================================
   رنگ‌های نمودار
   ====================================================== */
#define CLR_BG          RGB(18,  20,  26)   /* پس‌زمینه تاریک */
#define CLR_GRID        RGB(40,  45,  55)   /* خطوط شبکه */
#define CLR_LINE        RGB(30, 160, 255)   /* خط اصلی سرعت */
#define CLR_FILL_TOP    RGB(30, 140, 255)   /* گرادیان بالا */
#define CLR_FILL_BOT    RGB(10,  40,  90)   /* گرادیان پایین */
#define CLR_PEAK        RGB(255, 80,  80)   /* خط peak */
#define CLR_TEXT        RGB(200, 210, 230)  /* متن عادی */
#define CLR_TEXT_DIM    RGB(90, 100, 120)   /* متن کم‌رنگ */
#define CLR_BORDER      RGB(50,  55,  70)   /* حاشیه پنجره */

/* نام class پنجره */
#define SG_WNDCLASS "IDM_SpeedGraph_v1"

/* ======================================================
   ساختار داخلی پلاگین
   ====================================================== */
typedef struct {
    /* ring buffer سرعت */
    double   samples[SG_HISTORY_SECONDS];
    int      head;           /* ایندکس بعدی نوشتن */
    int      count;          /* تعداد نمونه‌های معتبر */
    double   peak_bps;
    double   sum_bps;        /* برای میانگین */
    long long total_samples; /* تعداد کل نمونه‌ها برای avg */

    /* engine */
    IDM_Engine *engine;

    /* GUI */
    HWND     hwnd;
    HWND     parent_hwnd;
    HFONT    font_small;
    HFONT    font_value;
    HDC      hdc_mem;        /* double buffer */
    HBITMAP  hbm_mem;
    int      bmp_w, bmp_h;

    /* timer */
    HANDLE   timer_thread;
    volatile BOOL timer_running;

    /* thread safety */
    CRITICAL_SECTION lock;

} SG_Priv;

/* ======================================================
   کمکی: فرمت سرعت
   ====================================================== */
static void fmt_speed(double bps, char *out, int sz) {
    if      (bps >= 1024.0*1024.0*1024.0)
        snprintf(out, sz, "%.2f GB/s", bps / (1024.0*1024.0*1024.0));
    else if (bps >= 1024.0*1024.0)
        snprintf(out, sz, "%.2f MB/s", bps / (1024.0*1024.0));
    else if (bps >= 1024.0)
        snprintf(out, sz, "%.1f KB/s", bps / 1024.0);
    else
        snprintf(out, sz, "%.0f B/s", bps);
}

/* ======================================================
   Timer thread: نمونه‌گیری سرعت هر ثانیه
   ====================================================== */
static DWORD WINAPI sg_timer_thread(LPVOID param) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)param;
    SG_Priv *p = (SG_Priv *)plug->priv;
    int brake_id = shutdown_brake_register("speed_graph_timer");

    while (p->timer_running) {
        Sleep(SG_SAMPLE_INTERVAL_MS);
        if (!p->timer_running) break;

        /* سرعت لحظه‌ای از engine */
        double spd = 0.0;
        if (p->engine)
            spd = engine_total_speed(p->engine);

        EnterCriticalSection(&p->lock);

        p->samples[p->head] = spd;
        p->head = (p->head + 1) % SG_HISTORY_SECONDS;
        if (p->count < SG_HISTORY_SECONDS) p->count++;

        if (spd > p->peak_bps) p->peak_bps = spd;
        p->sum_bps += spd;
        p->total_samples++;

        LeaveCriticalSection(&p->lock);

        /* به پنجره بگو redraw کن */
        if (p->hwnd && IsWindow(p->hwnd))
            InvalidateRect(p->hwnd, NULL, FALSE);
    }
    shutdown_brake_unregister(brake_id);
    return 0;
}

/* ======================================================
   Double-buffer: آماده‌سازی
   ====================================================== */
static void sg_ensure_buffer(SG_Priv *p, HDC hdc, int w, int h) {
    if (p->hdc_mem && p->bmp_w == w && p->bmp_h == h) return;

    if (p->hbm_mem) { SelectObject(p->hdc_mem, NULL); DeleteObject(p->hbm_mem); }
    if (p->hdc_mem) DeleteDC(p->hdc_mem);

    p->hdc_mem = CreateCompatibleDC(hdc);
    p->hbm_mem = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(p->hdc_mem, p->hbm_mem);
    p->bmp_w = w;
    p->bmp_h = h;
}

/* ======================================================
   رسم نمودار
   ====================================================== */
static void sg_draw(SG_Priv *p, HDC hdc, int cw, int ch) {
    sg_ensure_buffer(p, hdc, cw, ch);
    HDC dc = p->hdc_mem;

    /* ── پس‌زمینه ── */
    HBRUSH bg_br = CreateSolidBrush(CLR_BG);
    RECT rc = {0, 0, cw, ch};
    FillRect(dc, &rc, bg_br);
    DeleteObject(bg_br);

    /* ── ناحیه نمودار ── */
    int gx = SG_GRAPH_MARGIN_L;
    int gy = SG_GRAPH_MARGIN_T;
    int gw = cw - SG_GRAPH_MARGIN_L - SG_GRAPH_MARGIN_R;
    int gh = ch - SG_GRAPH_MARGIN_T - SG_GRAPH_MARGIN_B;
    if (gw < 10 || gh < 10) goto blit;

    /* ── snapshot داده ── */
    double snap[SG_HISTORY_SECONDS];
    int    snap_count, snap_head;
    double peak_bps, cur_bps = 0.0;
    /* avg_bps دیگه توی این popup کوچیک نشون داده نمی‌شه (جا نبود)
       — ولی همچنان از طریق sg_get_stats/out->avg_bps در دسترسه،
       فقط دیگه اینجا محاسبه نمی‌شه چون استفاده‌ای نداشت. */

    EnterCriticalSection(&p->lock);
    memcpy(snap, p->samples, sizeof(p->samples));
    snap_count = p->count;
    snap_head  = p->head;
    peak_bps   = p->peak_bps;
    /* آخرین نمونه: head-1 */
    if (snap_count > 0) {
        int last = (snap_head - 1 + SG_HISTORY_SECONDS) % SG_HISTORY_SECONDS;
        cur_bps = snap[last];
    }
    LeaveCriticalSection(&p->lock);

    /* ── حداکثر محور Y (با کمی فاصله بالا) ── */
    double y_max = peak_bps * 1.2;
    if (y_max < 1024.0) y_max = 1024.0; /* حداقل ۱ KB/s */

    /* ── خطوط شبکه افقی ──
       قبلاً ۴ خط بود؛ توی فضای کوچیک‌تر (پنجره‌ی جدید ۱۶۰×۶۴)
       ۴ برچسب روی هم می‌افتادن — به ۲ خط کاهش پیدا کرد (وسط و
       بالا) تا خوندنی بمونه. */
    HPEN grid_pen = CreatePen(PS_SOLID, 1, CLR_GRID);
    HPEN old_pen  = SelectObject(dc, grid_pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));

    for (int gi = 1; gi <= 2; gi++) {
        int yy = gy + gh - (int)(gh * gi / 2);
        MoveToEx(dc, gx, yy, NULL);
        LineTo  (dc, gx + gw, yy);

        /* برچسب محور Y */
        char lbl[32];
        fmt_speed(y_max * gi / 2, lbl, sizeof(lbl));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, CLR_TEXT_DIM);
        SelectObject(dc, p->font_small);
        RECT lr = {0, yy - 8, gx - 4, yy + 8};
        DrawTextA(dc, lbl, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    /* خطوط شبکه عمودی — قبلاً هر ۱۵ ثانیه (چند برچسب زمانی) بود؛
       توی پهنای جدید (۱۶۰px) حتی یه برچسب زمانی هم با نوار
       اطلاعات پایین (Current speed) تداخل پیدا می‌کرد. چون این
       محور زمان صرفاً informative بود (نه چیزی که تعامل‌پذیر
       باشه) و اکثر دانلود منیجرهای رقیب توی widget کوچیک‌شون
       اصلاً محور زمان نشون نمی‌دن، کامل حذف شد — شکل خط خودش
       روند رو نشون می‌ده. */

    SelectObject(dc, old_pen);
    DeleteObject(grid_pen);

    /* ── رسم ناحیه پر (fill under curve) با gradient ── */
    if (snap_count >= 2) {
        /* ساخت polygon برای fill */
        POINT *pts = (POINT *)malloc((snap_count + 2) * sizeof(POINT));
        if (pts) {
            for (int i = 0; i < snap_count; i++) {
                /* ایندکس sample از قدیمی‌ترین به جدیدترین */
                int idx = (snap_head - snap_count + i + SG_HISTORY_SECONDS)
                          % SG_HISTORY_SECONDS;
                int xx = gx + (int)((double)i / (snap_count - 1) * gw);
                int yy = gy + gh - (int)(snap[idx] / y_max * gh);
                if (yy < gy) yy = gy;
                pts[i].x = xx;
                pts[i].y = yy;
            }
            pts[snap_count].x = gx + gw;
            pts[snap_count].y = gy + gh;
            pts[snap_count+1].x = gx;
            pts[snap_count+1].y = gy + gh;

            /* gradient fill: چند لایه با شفافیت ترقیقی */
            for (int row = 0; row < gh; row++) {
                double t = (double)row / gh;  /* 0=بالا, 1=پایین */
                int r = (int)(GetRValue(CLR_FILL_TOP) * (1-t) + GetRValue(CLR_FILL_BOT) * t);
                int g = (int)(GetGValue(CLR_FILL_TOP) * (1-t) + GetGValue(CLR_FILL_BOT) * t);
                int b = (int)(GetBValue(CLR_FILL_TOP) * (1-t) + GetBValue(CLR_FILL_BOT) * t);
                HPEN rp = CreatePen(PS_SOLID, 1, RGB(r, g, b));
                SelectObject(dc, rp);
                MoveToEx(dc, gx, gy + row, NULL);
                LineTo(dc, gx + gw, gy + row);
                SelectObject(dc, GetStockObject(NULL_PEN));
                DeleteObject(rp);
            }

            /* clip fill به داخل polygon: از ایده RGNI استفاده می‌کنیم */
            HRGN fill_rgn = CreatePolygonRgn(pts, snap_count + 2, WINDING);
            HBRUSH fill_br = CreateSolidBrush(CLR_BG);
            /* ناحیه بالای curve رو پاک کن */
            HRGN graph_rgn = CreateRectRgn(gx, gy, gx+gw, gy+gh);
            CombineRgn(fill_rgn, graph_rgn, fill_rgn, RGN_DIFF);
            FillRgn(dc, fill_rgn, fill_br);
            DeleteObject(fill_rgn);
            DeleteObject(graph_rgn);
            DeleteObject(fill_br);

            free(pts);
        }
    }

    /* ── خط اصلی curve ── */
    if (snap_count >= 2) {
        HPEN line_pen = CreatePen(PS_SOLID, 2, CLR_LINE);
        SelectObject(dc, line_pen);

        BOOL first = TRUE;
        for (int i = 0; i < snap_count; i++) {
            int idx = (snap_head - snap_count + i + SG_HISTORY_SECONDS)
                      % SG_HISTORY_SECONDS;
            int xx = gx + (int)((double)i / (snap_count - 1) * gw);
            int yy = gy + gh - (int)(snap[idx] / y_max * gh);
            if (yy < gy) yy = gy;
            if (first) { MoveToEx(dc, xx, yy, NULL); first = FALSE; }
            else         LineTo(dc, xx, yy);
        }

        SelectObject(dc, old_pen);
        DeleteObject(line_pen);
    }

    /* ── خط peak ── */
    if (peak_bps > 0) {
        int peak_y = gy + gh - (int)(peak_bps / y_max * gh);
        if (peak_y < gy) peak_y = gy;
        HPEN pk_pen = CreatePen(PS_DOT, 1, CLR_PEAK);
        SelectObject(dc, pk_pen);
        MoveToEx(dc, gx, peak_y, NULL);
        LineTo(dc, gx + gw, peak_y);
        SelectObject(dc, old_pen);
        DeleteObject(pk_pen);
    }

    /* ── حاشیه ناحیه نمودار ── */
    HPEN border_pen = CreatePen(PS_SOLID, 1, CLR_BORDER);
    SelectObject(dc, border_pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, gx, gy, gx + gw + 1, gy + gh + 1);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);

    /* ── نوار اطلاعات پایین ──
       قبلاً Current/Peak/Avg سه‌تایی کنار هم با آفست‌های ثابت
       پیکسلی (120، 260) رسم می‌شدن — فرضش یه پنجره‌ی حداقل ۴۰۰px
       عریض بود. توی پهنای جدید (۱۶۰px) این سه‌تا کاملاً از پنجره
       بیرون می‌زدن. چون واقعاً جای کافی برای هر سه با فونت
       خوانا نیست، فقط Current (مهم‌ترین عدد برای یه نگاه سریع،
       دقیقاً همون چیزی که widget کوچیک بیشتر رقبا هم نشون
       می‌ده) نگه داشته شد؛ Peak/Avg از این popup کوچیک حذف شدن —
       اگه لازمشون داری، بگو تا یه راه دیگه (مثلاً tooltip موقع
       hover) براش اضافه کنم. */
    {
        char cur_str[32];
        fmt_speed(cur_bps, cur_str, sizeof(cur_str));

        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, p->font_value);
        SetTextColor(dc, CLR_LINE);
        RECT cr = {gx, gy + gh + 2, cw - 4, ch};
        DrawTextA(dc, cur_str, -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

blit:
    /* ── blit به hdc اصلی ── */
    BitBlt(hdc, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
}

/* ======================================================
   WndProc پنجره نمودار
   ====================================================== */
static LRESULT CALLBACK sg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    SG_Priv *p = plug ? (SG_Priv *)plug->priv : NULL;

    switch (msg) {
    case WM_PAINT: {
        if (!p) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        sg_draw(p, hdc, rc.right, rc.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        if (!p) break;
        /* بافر قدیمی رو پاک کن تا دوباره ساخته بشه */
        if (p->hbm_mem) { DeleteObject(p->hbm_mem); p->hbm_mem = NULL; p->bmp_w = 0; }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* double-buffer خودش handle می‌کنه */
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0; /* حذف نمیشه، فقط مخفی */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ======================================================
   ثبت Window Class
   ====================================================== */
static BOOL sg_register_class(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = SG_WNDCLASS;
    wc.lpfnWndProc   = sg_wnd_proc;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    return (RegisterClassExA(&wc) != 0) || (GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

/* ======================================================
   Plugin hooks
   ====================================================== */
static int sg_on_init(void) { return 0; }
static void sg_on_destroy(void) {}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *speed_graph_plugin_create(HWND parent_hwnd,
                                             IDM_Engine *engine) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(IDM_PluginHooks));
    if (!plug) return NULL;

    SG_Priv *p = (SG_Priv *)calloc(1, sizeof(SG_Priv));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->engine      = engine;
    p->parent_hwnd = parent_hwnd;
    p->timer_running = FALSE;

    /* فونت‌ها */
    p->font_small = CreateFontA(11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    p->font_value = CreateFontA(13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    /* ثبت class و ساخت پنجره */
    sg_register_class();
    p->hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        SG_WNDCLASS,
        "Speed Graph",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
        100, 100,
        SG_WINDOW_W + 16,         /* تنظیم برای border */
        SG_WINDOW_H + 38,
        parent_hwnd,
        NULL,
        GetModuleHandleA(NULL),
        NULL
    );

    if (!p->hwnd) {
        DeleteCriticalSection(&p->lock);
        if (p->font_small) DeleteObject(p->font_small);
        if (p->font_value) DeleteObject(p->font_value);
        free(p); free(plug);
        return NULL;
    }

    /* ذخیره pointer پلاگین در پنجره */
    plug->priv = p;
    SetWindowLongPtrA(p->hwnd, GWLP_USERDATA, (LONG_PTR)plug);

    /* fill hooks */
    plug->name        = "speed_graph";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = sg_on_init;
    plug->on_destroy  = sg_on_destroy;
    /* بقیه hookها NULL هستن - این پلاگین فقط poll می‌کنه */

    /* timer thread رو شروع کن */
    p->timer_running = TRUE;
    p->timer_thread = CreateThread(NULL, 0, sg_timer_thread, plug, 0, NULL);
    if (!p->timer_thread) {
        p->timer_running = FALSE;
    }

    LOGI("speed_graph", "plugin created (hwnd=%p)", (void*)p->hwnd);
    return plug;
}

void speed_graph_plugin_signal_stop(IDM_PluginHooks *plug) {
    if (!plug || !plug->priv) return;
    ((SG_Priv *)plug->priv)->timer_running = FALSE;
}

void speed_graph_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    SG_Priv *p = (SG_Priv *)plug->priv;
    if (!p) { free(plug); return; }

    /* timer رو متوقف کن */
    p->timer_running = FALSE;
    if (p->timer_thread) {
        WaitForSingleObject(p->timer_thread, 3000);
        CloseHandle(p->timer_thread);
    }

    /* پنجره رو ببند */
    if (p->hwnd && IsWindow(p->hwnd))
        DestroyWindow(p->hwnd);

    /* منابع GDI */
    if (p->hbm_mem) DeleteObject(p->hbm_mem);
    if (p->hdc_mem) DeleteDC(p->hdc_mem);
    if (p->font_small) DeleteObject(p->font_small);
    if (p->font_value) DeleteObject(p->font_value);

    DeleteCriticalSection(&p->lock);
    free(p);
    free(plug);
}

void speed_graph_show(IDM_PluginHooks *plug, BOOL visible) {
    if (!plug) return;
    SG_Priv *p = (SG_Priv *)plug->priv;
    if (!p || !p->hwnd) return;
    ShowWindow(p->hwnd, visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        SetForegroundWindow(p->hwnd);
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
}

BOOL speed_graph_visible(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    SG_Priv *p = (SG_Priv *)plug->priv;
    if (!p || !p->hwnd) return FALSE;
    return IsWindowVisible(p->hwnd);
}

void speed_graph_get_stats(IDM_PluginHooks *plug, SG_Stats *out) {
    if (!plug || !out) return;
    SG_Priv *p = (SG_Priv *)plug->priv;
    if (!p) return;

    EnterCriticalSection(&p->lock);
    memcpy(out->samples, p->samples, sizeof(p->samples));
    out->head         = p->head;
    out->sample_count = p->count;
    out->peak_bps     = p->peak_bps;
    out->avg_bps      = (p->total_samples > 0)
                        ? p->sum_bps / (double)p->total_samples : 0.0;
    out->current_bps  = 0.0;
    if (p->count > 0) {
        int last = (p->head - 1 + SG_HISTORY_SECONDS) % SG_HISTORY_SECONDS;
        out->current_bps = p->samples[last];
    }
    LeaveCriticalSection(&p->lock);
}
