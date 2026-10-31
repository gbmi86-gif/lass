/* plugin_notify.c
   ─────────────────────────────────────────────────────────────
   Download Notification Plugin

   دو حالت:
     BALLOON: Shell_NotifyIcon با NIF_INFO — balloon از tray
     TOAST:   یه پنجره کوچیک GDI در گوشه پایین‌راست
              که بعد از duration_ms خودکار محو میشه
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "security_fixes.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_notify.h"
#include "../src/i18n.h"
#include "../src/lass_log.h"

/* ── رنگ‌های toast ── */
#define TOAST_CLR_BG_OK    RGB(30,  130, 60)   /* سبز — complete */
#define TOAST_CLR_BG_FAIL  RGB(160, 40,  40)   /* قرمز — failed */
#define TOAST_CLR_BG_INFO  RGB(30,  90,  160)  /* آبی — info */
#define TOAST_CLR_TEXT     RGB(255, 255, 255)
#define TOAST_W            320
#define TOAST_H            72
#define TOAST_MARGIN       12

#define TOAST_WNDCLASS "LassToastNotify"
#define TOAST_TIMER_ID 601

typedef struct {
    NotifyConfig cfg;
    HWND         tray_hwnd;
    UINT         tray_id;
    HWND         toast_hwnd;
    COLORREF     toast_color;
    char         toast_title[128];
    char         toast_body[256];
    HFONT        font_title;
    HFONT        font_body;
    CRITICAL_SECTION lock;
} Notify_Priv;

/* ======================================================
   Toast Window
   ====================================================== */
static LRESULT CALLBACK toast_wnd_proc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp) {
    Notify_Priv *p = (Notify_Priv *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        /* پس‌زمینه */
        HBRUSH bg = CreateSolidBrush(p ? p->toast_color : TOAST_CLR_BG_INFO);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        /* نوار رنگی سمت چپ */
        RECT bar = {0, 0, 5, rc.bottom};
        HBRUSH bar_br = CreateSolidBrush(RGB(255,255,255));
        FillRect(hdc, &bar, bar_br);
        DeleteObject(bar_br);

        if (p) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, TOAST_CLR_TEXT);

            /* عنوان */
            SelectObject(hdc, p->font_title);
            RECT tr = {TOAST_MARGIN+6, 10, rc.right-8, 32};
            DrawTextA(hdc, p->toast_title, -1, &tr,
                      DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

            /* بدنه */
            SelectObject(hdc, p->font_body);
            RECT br = {TOAST_MARGIN+6, 34, rc.right-8, rc.bottom-8};
            DrawTextA(hdc, p->toast_body, -1, &br,
                      DT_LEFT|DT_WORDBREAK|DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == TOAST_TIMER_ID) {
            KillTimer(hwnd, TOAST_TIMER_ID);
            /* fade: مخفی کن */
            AnimateWindow(hwnd, 200, AW_HIDE|AW_BLEND);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* کلیک روی toast → فوری ببند */
        KillTimer(hwnd, TOAST_TIMER_ID);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void register_toast_class(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = TOAST_WNDCLASS;
    wc.lpfnWndProc   = toast_wnd_proc;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.style         = CS_HREDRAW|CS_VREDRAW;
    RegisterClassExA(&wc); /* اگه قبلاً ثبت شده اشکالی نیست */
}

static void show_toast(Notify_Priv *p, NotifyEvent evt,
                        const char *title, const char *body) {
    if (!p) return;

    /* color بر اساس event */
    switch (evt) {
    case NOTIFY_COMPLETE:   p->toast_color = TOAST_CLR_BG_OK;   break;
    case NOTIFY_FAILED:     p->toast_color = TOAST_CLR_BG_FAIL; break;
    default:                p->toast_color = TOAST_CLR_BG_INFO; break;
    }

    safe_strncpy(p->toast_title, title ? title : "", sizeof(p->toast_title));
    safe_strncpy(p->toast_body, body ? body : "", sizeof(p->toast_body));

    /* موقعیت: گوشه پایین‌راست */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    /* taskbar height حدوداً ۴۸px */
    int x = sw - TOAST_W - 12;
    int y = sh - TOAST_H - 52;

    if (!p->toast_hwnd || !IsWindow(p->toast_hwnd)) {
        register_toast_class();
        p->toast_hwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            TOAST_WNDCLASS, "",
            WS_POPUP,
            x, y, TOAST_W, TOAST_H,
            NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (p->toast_hwnd)
            SetWindowLongPtrA(p->toast_hwnd, GWLP_USERDATA, (LONG_PTR)p);
    }

    if (!p->toast_hwnd) return;

    /* شفافیت */
    SetLayeredWindowAttributes(p->toast_hwnd, 0, 230, LWA_ALPHA);
    SetWindowPos(p->toast_hwnd, HWND_TOPMOST, x, y,
                 TOAST_W, TOAST_H, SWP_NOACTIVATE);

    /* نمایش با animation */
    ShowWindow(p->toast_hwnd, SW_SHOWNOACTIVATE);
    AnimateWindow(p->toast_hwnd, 150, AW_SLIDE|AW_VER_NEGATIVE);
    InvalidateRect(p->toast_hwnd, NULL, TRUE);

    /* timer برای بستن */
    SetTimer(p->toast_hwnd, TOAST_TIMER_ID,
             p->cfg.duration_ms > 0 ? p->cfg.duration_ms : 4000, NULL);

    /* صدا */
    if (p->cfg.sound) {
        switch (evt) {
        case NOTIFY_COMPLETE: MessageBeep(MB_ICONINFORMATION); break;
        case NOTIFY_FAILED:   MessageBeep(MB_ICONEXCLAMATION); break;
        default:              MessageBeep(0xFFFFFFFF);          break;
        }
    }
}

/* ======================================================
   Balloon از system tray
   ====================================================== */
static void show_balloon(Notify_Priv *p, NotifyEvent evt,
                          const char *title, const char *body) {
    if (!p || !p->tray_hwnd) return;

    NOTIFYICONDATAA nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = p->tray_hwnd;
    nid.uID         = p->tray_id;
    nid.uFlags      = NIF_INFO;
    nid.uTimeout    = (UINT)(p->cfg.duration_ms > 0 ? p->cfg.duration_ms : 4000);

    switch (evt) {
    case NOTIFY_COMPLETE:
        nid.dwInfoFlags = NIIF_INFO;
        break;
    case NOTIFY_FAILED:
        nid.dwInfoFlags = NIIF_WARNING;
        break;
    default:
        nid.dwInfoFlags = NIIF_INFO;
        break;
    }

    safe_strncpy(nid.szInfoTitle, title ? title : "Lass", sizeof(nid.szInfoTitle));
    safe_strncpy(nid.szInfo, body ? body : "", sizeof(nid.szInfo));

    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

/* ======================================================
   Plugin hooks
   ====================================================== */
static int  notify_on_init   (void) { return 0; }
static void notify_on_destroy(void) {}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *notify_plugin_create(HWND tray_hwnd, UINT tray_id) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    Notify_Priv *p = (Notify_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->tray_hwnd = tray_hwnd;
    p->tray_id   = tray_id;

    /* تنظیمات پیش‌فرض */
    p->cfg.enabled     = TRUE;
    p->cfg.style       = NOTIFY_STYLE_TOAST;
    p->cfg.duration_ms = 4000;
    p->cfg.sound       = TRUE;

    /* فونت‌ها */
    p->font_title = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH|FF_SWISS, "Segoe UI");
    p->font_body  = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH|FF_SWISS, "Segoe UI");

    plug->priv        = p;
    plug->name        = "download_notify";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = notify_on_init;
    plug->on_destroy  = notify_on_destroy;

    LOGI(LOG_MOD_NOTIFY, "plugin created");
    return plug;
}

void notify_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    Notify_Priv *p = (Notify_Priv *)plug->priv;
    if (p) {
        if (p->toast_hwnd && IsWindow(p->toast_hwnd)) {
            /* حیاتی: اول timer رو بکش، بعد USERDATA pointer رو
               صفر کن، بعد پنجره رو نابود کن.
               بدون KillTimer: اگه timer هنوز فعال باشه و یه
               WM_PAINT یا WM_TIMER قبل از DestroyWindow در صف
               باشه، toast_wnd_proc با p آزاد‌شده کار می‌کنه
               (use-after-free = segfault).
               با صفر کردن GWLP_USERDATA قبل از DestroyWindow:
               هر پیامی که DispatchMessageA بعداً صدا بزنه، p==NULL
               می‌گیره و null-guard های موجود اونو ایمن رد می‌کنن. */
            KillTimer(p->toast_hwnd, TOAST_TIMER_ID);
            SetWindowLongPtrA(p->toast_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(p->toast_hwnd);
        }
        if (p->font_title) DeleteObject(p->font_title);
        if (p->font_body)  DeleteObject(p->font_body);
        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}

void notify_plugin_configure(IDM_PluginHooks *plug,
                               const NotifyConfig *cfg) {
    if (!plug || !cfg) return;
    Notify_Priv *p = (Notify_Priv *)plug->priv;
    if (!p) return;
    EnterCriticalSection(&p->lock);
    p->cfg = *cfg;
    LeaveCriticalSection(&p->lock);
}

void notify_plugin_get_config(IDM_PluginHooks *plug, NotifyConfig *out) {
    if (!plug || !out) return;
    Notify_Priv *p = (Notify_Priv *)plug->priv;
    if (!p) return;
    EnterCriticalSection(&p->lock);
    *out = p->cfg;
    LeaveCriticalSection(&p->lock);
}

void notify_send(IDM_PluginHooks *plug, NotifyEvent evt,
                  const char *filename, const char *detail) {
    if (!plug) return;
    Notify_Priv *p = (Notify_Priv *)plug->priv;
    if (!p || !p->cfg.enabled) return;

    /* عنوان و بدنه */
    char title[128] = {0};
    char body[256]  = {0};

    switch (evt) {
    case NOTIFY_COMPLETE:
        snprintf(title, sizeof(title), "Download Complete");
        snprintf(body,  sizeof(body),  "%s",
                 filename ? filename : "");
        break;
    case NOTIFY_FAILED:
        snprintf(title, sizeof(title), "Download Failed");
        snprintf(body,  sizeof(body),  "%s\n%s",
                 filename ? filename : "",
                 detail   ? detail   : "");
        break;
    case NOTIFY_BATCH_DONE:
        snprintf(title, sizeof(title), "Batch Complete");
        snprintf(body,  sizeof(body),  "All downloads in group finished.");
        break;
    case NOTIFY_UNSCANNED:
        snprintf(title, sizeof(title), "%s", i18n_str(STR_NOTIFY_UNSCANNED_TITLE));
        snprintf(body,  sizeof(body),  "%s\n%s",
                 filename ? filename : "",
                 i18n_str(STR_NOTIFY_UNSCANNED_BODY));
        break;
    default:
        snprintf(title, sizeof(title), "Lass");
        snprintf(body,  sizeof(body),  "%s", detail ? detail : "");
        break;
    }

    if (p->cfg.style == NOTIFY_STYLE_TOAST)
        show_toast(p, evt, title, body);
    else
        show_balloon(p, evt, title, body);
}

void notify_on_download_complete(IDM_PluginHooks *plug,
                                  const char *filename, BOOL success) {
    notify_send(plug,
                success ? NOTIFY_COMPLETE : NOTIFY_FAILED,
                filename, NULL);
}

void notify_set_tray(IDM_PluginHooks *plug, HWND hwnd, UINT id) {
    if (!plug) return;
    Notify_Priv *p = (Notify_Priv *)plug->priv;
    if (!p) return;
    p->tray_hwnd = hwnd;
    p->tray_id   = id;
}
