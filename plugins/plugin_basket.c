/* plugin_basket.c
   ─────────────────────────────────────────────────────────────
   Floating Download Basket

   ویژگی‌های فنی:
     - WS_EX_TOPMOST + WS_EX_TOOLWINDOW (همیشه رو و بدون taskbar)
     - WS_EX_LAYERED برای شفافیت (alpha=220)
     - Drag با WM_NCHITTEST → HTCAPTION
     - Drag & Drop با RegisterDragDrop / IDropTarget
     - Paste با Ctrl+V در WM_KEYDOWN
     - Double-click → بزرگ/کوچک toggle
     - GDI rendering با double-buffer
     - هر ۵۰۰ms از engine سرعت/تعداد می‌خونه
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/url_secure.h"
#include "plugin_basket.h"
#include "../src/lass_log.h"

/* ── ابعاد ── */
#define BK_NORMAL_W  130
#define BK_NORMAL_H   80
#define BK_COMPACT_W  48
#define BK_COMPACT_H  48

/* ── رنگ‌ها ── */
#define BK_CLR_BG_IDLE     RGB(28,  32,  42)
#define BK_CLR_BG_HOVER    RGB(35,  45,  65)
#define BK_CLR_BG_ACTIVE   RGB(20,  80, 170)
#define BK_CLR_BORDER      RGB(60,  80, 130)
#define BK_CLR_BORDER_HOT  RGB(80, 140, 255)
#define BK_CLR_TEXT        RGB(210, 220, 240)
#define BK_CLR_TEXT_DIM    RGB(100, 115, 140)
#define BK_CLR_SPEED       RGB(80,  200, 120)
#define BK_CLR_ICON        RGB(80,  160, 255)
#define BK_ALPHA           220   /* شفافیت لایه‌ای (0-255) */

#define BK_WNDCLASS  "IDM_Basket_v1"
#define BK_TIMER_ID  501
#define BK_TIMER_MS  500

/* ======================================================
   ساختار داخلی
   ====================================================== */
typedef struct {
    HWND          hwnd;
    HWND          owner_hwnd;
    IDM_Engine   *engine;
    Basket_DropCb on_drop;
    void         *userdata;
    BasketMode    mode;

    /* وضعیت hover */
    BOOL          hover;
    BOOL          drag_over;   /* DnD hover */

    /* آمار از engine */
    int           active_count;
    double        total_speed_bps;

    /* double buffer */
    HDC           hdc_mem;
    HBITMAP       hbm_mem;
    int           bmp_w, bmp_h;

    /* فونت‌ها */
    HFONT         font_small;
    HFONT         font_icon;   /* برای آیکون متنی */

    CRITICAL_SECTION lock;
} BK_Priv;

/* ── forward ── */
static void bk_draw(BK_Priv *p, HDC hdc, int w, int h);
static void bk_ensure_buffer(BK_Priv *p, HDC hdc, int w, int h);

/* ======================================================
   IDropTarget implementation
   ====================================================== */
typedef struct {
    IDropTarget      idt;      /* باید اول باشه */
    LONG             ref;
    BK_Priv         *priv;
    IDM_PluginHooks *plug;
} BK_DropTarget;

static HRESULT STDMETHODCALLTYPE bk_dt_QueryInterface(
    IDropTarget *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDropTarget)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bk_dt_AddRef(IDropTarget *This) {
    return InterlockedIncrement(&((BK_DropTarget*)This)->ref);
}
static ULONG STDMETHODCALLTYPE bk_dt_Release(IDropTarget *This) {
    LONG r = InterlockedDecrement(&((BK_DropTarget*)This)->ref);
    if (r == 0) free(This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE bk_dt_DragEnter(
    IDropTarget *This, IDataObject *pDO, DWORD grfKeyState,
    POINTL pt, DWORD *pdwEffect) {
    (void)pDO; (void)grfKeyState; (void)pt;
    BK_DropTarget *self = (BK_DropTarget*)This;
    self->priv->drag_over = TRUE;
    InvalidateRect(self->priv->hwnd, NULL, FALSE);
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bk_dt_DragOver(
    IDropTarget *This, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect) {
    (void)This; (void)grfKeyState; (void)pt;
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bk_dt_DragLeave(IDropTarget *This) {
    BK_DropTarget *self = (BK_DropTarget*)This;
    self->priv->drag_over = FALSE;
    InvalidateRect(self->priv->hwnd, NULL, FALSE);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE bk_dt_Drop(
    IDropTarget *This, IDataObject *pDO, DWORD grfKeyState,
    POINTL pt, DWORD *pdwEffect) {
    (void)grfKeyState; (void)pt;
    BK_DropTarget *self = (BK_DropTarget*)This;
    self->priv->drag_over = FALSE;

    *pdwEffect = DROPEFFECT_NONE;

    FORMATETC fmt = { CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg = {0};

    /* تلاش برای UNICODETEXT اول */
    FORMATETC fmtW = { CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    char url[2048] = {0};

    if (SUCCEEDED(pDO->lpVtbl->GetData(pDO, &fmtW, &stg))) {
        wchar_t *wt = (wchar_t *)GlobalLock(stg.hGlobal);
        if (wt) {
            WideCharToMultiByte(CP_ACP, 0, wt, -1, url, sizeof(url)-1, NULL, NULL);
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
    } else if (SUCCEEDED(pDO->lpVtbl->GetData(pDO, &fmt, &stg))) {
        char *t = (char *)GlobalLock(stg.hGlobal);
        if (t) strncpy(url, t, sizeof(url)-1);
        GlobalUnlock(stg.hGlobal);
        ReleaseStgMedium(&stg);
    }

    /* trim */
    char *end = url + strlen(url) - 1;
    while (end > url && (*end == '\r' || *end == '\n' || *end == ' '))
        *end-- = '\0';

    if (strlen(url) > 7 &&
        (strncmp(url, "http://", 7) == 0 ||
         strncmp(url, "https://", 8) == 0 ||
         strncmp(url, "ftp://", 6) == 0)) {
        if (self->priv->on_drop)
            self->priv->on_drop(url, self->priv->userdata);
        *pdwEffect = DROPEFFECT_COPY;
        char red_url[256];
        url_redact_for_log(url, red_url, sizeof(red_url));
        LOGI(LOG_MOD_BASKET, "dropped URL: %.60s", red_url);
    }

    InvalidateRect(self->priv->hwnd, NULL, FALSE);
    return S_OK;
}

static IDropTargetVtbl g_bk_dt_vtbl = {
    bk_dt_QueryInterface,
    bk_dt_AddRef,
    bk_dt_Release,
    bk_dt_DragEnter,
    bk_dt_DragOver,
    bk_dt_DragLeave,
    bk_dt_Drop,
};

/* ======================================================
   فرمت سرعت کوتاه
   ====================================================== */
static void bk_fmt_speed(double bps, char *out, int sz) {
    if      (bps >= 1024.0*1024.0) snprintf(out, sz, "%.1fM", bps/1024.0/1024.0);
    else if (bps >= 1024.0)         snprintf(out, sz, "%.0fK", bps/1024.0);
    else if (bps > 0)               snprintf(out, sz, "%.0fB", bps);
    else                            snprintf(out, sz, "---");
}

/* ======================================================
   رسم GDI
   ====================================================== */
static void bk_ensure_buffer(BK_Priv *p, HDC hdc, int w, int h) {
    if (p->hdc_mem && p->bmp_w == w && p->bmp_h == h) return;
    if (p->hbm_mem) { SelectObject(p->hdc_mem, NULL); DeleteObject(p->hbm_mem); }
    if (p->hdc_mem) DeleteDC(p->hdc_mem);
    p->hdc_mem = CreateCompatibleDC(hdc);
    p->hbm_mem = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(p->hdc_mem, p->hbm_mem);
    p->bmp_w = w; p->bmp_h = h;
}

static void bk_draw(BK_Priv *p, HDC hdc, int w, int h) {
    bk_ensure_buffer(p, hdc, w, h);
    HDC dc = p->hdc_mem;

    COLORREF bg = p->drag_over ? BK_CLR_BG_ACTIVE :
                  p->hover     ? BK_CLR_BG_HOVER  : BK_CLR_BG_IDLE;
    COLORREF border = (p->hover || p->drag_over) ? BK_CLR_BORDER_HOT
                                                  : BK_CLR_BORDER;

    HBRUSH bg_br = CreateSolidBrush(bg);
    RECT rc = {0, 0, w, h};
    FillRect(dc, &rc, bg_br);
    DeleteObject(bg_br);

    /* ── حاشیه گرد ── */
    HPEN border_pen = CreatePen(PS_SOLID, 2, border);
    HPEN old_pen    = SelectObject(dc, border_pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 1, 1, w-1, h-1, 10, 10);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);

    SetBkMode(dc, TRANSPARENT);

    if (p->mode == BASKET_MODE_COMPACT) {
        /* ── حالت فشرده: فقط آیکون + تعداد ── */
        /* آیکون پیکان دانلود */
        SelectObject(dc, p->font_icon);
        SetTextColor(dc, p->active_count > 0 ? BK_CLR_SPEED : BK_CLR_ICON);
        RECT ir = {0, 4, w, h - 12};
        DrawTextA(dc, "\x1F", -1, &ir, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        if (p->active_count > 0) {
            char cnt[16];  /* بزرگ‌تر از 8 تا overflow نشه */
            snprintf(cnt, sizeof(cnt), "%d", p->active_count);
            SelectObject(dc, p->font_small);
            SetTextColor(dc, BK_CLR_TEXT);
            RECT cr = {0, h - 14, w, h - 2};
            DrawTextA(dc, cnt, -1, &cr, DT_CENTER | DT_SINGLELINE);
        }
    } else {
        /* ── حالت عادی ── */
        /* عنوان */
        SelectObject(dc, p->font_small);
        SetTextColor(dc, BK_CLR_TEXT_DIM);
        RECT tr = {8, 5, w - 8, 20};
        DrawTextA(dc, "Lass Basket", -1, &tr, DT_LEFT | DT_SINGLELINE);

        /* خط جداکننده */
        HPEN sep_pen = CreatePen(PS_SOLID, 1, BK_CLR_BORDER);
        SelectObject(dc, sep_pen);
        MoveToEx(dc, 8, 22, NULL);
        LineTo(dc, w - 8, 22);
        SelectObject(dc, old_pen);
        DeleteObject(sep_pen);

        /* تعداد دانلود */
        char line1[48];
        snprintf(line1, sizeof(line1), "Active: %d", p->active_count);
        SetTextColor(dc, BK_CLR_TEXT);
        RECT r1 = {8, 26, w - 8, 42};
        DrawTextA(dc, line1, -1, &r1, DT_LEFT | DT_SINGLELINE);

        /* سرعت */
        char spd[32];
        bk_fmt_speed(p->total_speed_bps, spd, sizeof(spd));
        char line2[48];
        snprintf(line2, sizeof(line2), "Speed: %s/s", spd);
        SetTextColor(dc, BK_CLR_SPEED);
        RECT r2 = {8, 42, w - 8, 58};
        DrawTextA(dc, line2, -1, &r2, DT_LEFT | DT_SINGLELINE);

        /* راهنما drop */
        SetTextColor(dc, p->drag_over ? BK_CLR_TEXT : BK_CLR_TEXT_DIM);
        RECT r3 = {4, 58, w - 4, 76};
        DrawTextA(dc, p->drag_over ? "Drop to download!" : "Drop URL here",
                  -1, &r3, DT_CENTER | DT_SINGLELINE);
    }

    BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
}

/* ======================================================
   WndProc
   ====================================================== */
static LRESULT CALLBACK bk_wnd_proc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp) {
    BK_Priv *p = (BK_Priv *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        if (!p) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        bk_draw(p, hdc, rc.right, rc.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    /* ── drag پنجره با کلیک روی هر جایی ── */
    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcA(hwnd, msg, wp, lp);
        return (def == HTCLIENT) ? HTCAPTION : def;
    }

    /* ── hover tracking ── */
    case WM_MOUSEMOVE:
        if (p && !p->hover) {
            p->hover = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (p) { p->hover = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;

    /* ── double-click → toggle mode ── */
    case WM_LBUTTONDBLCLK: {
        if (!p) break;
        BasketMode new_mode = (p->mode == BASKET_MODE_NORMAL)
                              ? BASKET_MODE_COMPACT : BASKET_MODE_NORMAL;
        int nw = (new_mode == BASKET_MODE_NORMAL) ? BK_NORMAL_W : BK_COMPACT_W;
        int nh = (new_mode == BASKET_MODE_NORMAL) ? BK_NORMAL_H : BK_COMPACT_H;

        RECT wr; GetWindowRect(hwnd, &wr);
        p->mode = new_mode;
        SetWindowPos(hwnd, NULL, wr.left, wr.top, nw, nh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    /* ── Ctrl+V: paste URL ── */
    case WM_KEYDOWN:
        if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (!OpenClipboard(hwnd)) break;
            HANDLE hText = GetClipboardData(CF_TEXT);
            if (hText) {
                char *txt = (char *)GlobalLock(hText);
                if (txt && p && p->on_drop) {
                    /* trim */
                    char url[2048] = {0};
                    strncpy(url, txt, sizeof(url)-1);
                    char *end = url + strlen(url) - 1;
                    while (end > url && (*end=='\r'||*end=='\n'||*end==' '))
                        *end-- = '\0';
                    if (strncmp(url,"http",4)==0 || strncmp(url,"ftp",3)==0)
                        p->on_drop(url, p->userdata);
                }
                GlobalUnlock(hText);
            }
            CloseClipboard();
            return 0;
        }
        break;

    /* ── timer: آپدیت آمار ── */
    case WM_TIMER:
        if (wp == BK_TIMER_ID && p && p->engine) {
            int old_active = p->active_count;
            double old_speed = p->total_speed_bps;

            /* شمارش دانلودهای فعال */
            int cnt = 0;
            EnterCriticalSection(&p->engine->lock);
            for (IDM_Download *dl = p->engine->queue_head; dl; dl = dl->next)
                if (dl->state == DL_DOWNLOADING) cnt++;
            LeaveCriticalSection(&p->engine->lock);

            p->active_count   = cnt;
            p->total_speed_bps = engine_total_speed(p->engine);

            if (p->active_count != old_active ||
                (int)(p->total_speed_bps/1024) != (int)(old_speed/1024))
                InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_SIZE:
        if (p) {
            if (p->hbm_mem) {
                DeleteObject(p->hbm_mem);
                p->hbm_mem = NULL; p->bmp_w = 0;
            }
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ======================================================
   ثبت Class
   ====================================================== */
static BOOL bk_register_class(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = BK_WNDCLASS;
    wc.lpfnWndProc   = bk_wnd_proc;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.style         = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    return (RegisterClassExA(&wc) != 0) ||
           (GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

/* ── Plugin hooks ── */
static int  bk_on_init   (void) { return 0; }
static void bk_on_destroy(void) {}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *basket_plugin_create(HWND owner_hwnd,
                                       IDM_Engine *engine,
                                       Basket_DropCb on_drop,
                                       void *userdata) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    BK_Priv *p = (BK_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->owner_hwnd = owner_hwnd;
    p->engine     = engine;
    p->on_drop    = on_drop;
    p->userdata   = userdata;
    p->mode       = BASKET_MODE_NORMAL;

    /* فونت‌ها */
    p->font_small = CreateFontA(11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH|FF_SWISS, "Segoe UI");
    p->font_icon  = CreateFontA(22, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 SYMBOL_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH|FF_DONTCARE, "Marlett");

    bk_register_class();
    OleInitialize(NULL); /* برای DnD */

    /* محاسبه موقعیت: گوشه پایین‌راست */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int wx = sw - BK_NORMAL_W - 16;
    int wy = sh - BK_NORMAL_H - 48;

    p->hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        BK_WNDCLASS,
        "Lass Basket",
        WS_POPUP,
        wx, wy, BK_NORMAL_W, BK_NORMAL_H,
        owner_hwnd, NULL, GetModuleHandleA(NULL), NULL
    );

    if (!p->hwnd) {
        DeleteCriticalSection(&p->lock);
        if (p->font_small) DeleteObject(p->font_small);
        if (p->font_icon)  DeleteObject(p->font_icon);
        free(p); free(plug);
        return NULL;
    }

    /* شفافیت لایه‌ای */
    SetLayeredWindowAttributes(p->hwnd, 0, BK_ALPHA, LWA_ALPHA);

    SetWindowLongPtrA(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
    plug->priv = p;

    /* ثبت IDropTarget */
    BK_DropTarget *dt = (BK_DropTarget *)calloc(1, sizeof(*dt));
    if (dt) {
        dt->idt.lpVtbl = &g_bk_dt_vtbl;
        dt->ref  = 1;
        dt->priv = p;
        dt->plug = plug;
        RegisterDragDrop(p->hwnd, (IDropTarget*)dt);
    }

    /* timer */
    SetTimer(p->hwnd, BK_TIMER_ID, BK_TIMER_MS, NULL);

    plug->name        = "download_basket";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = bk_on_init;
    plug->on_destroy  = bk_on_destroy;

    LOGI(LOG_MOD_BASKET, "plugin created (hwnd=%p)", (void*)p->hwnd);
    return plug;
}

void basket_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    BK_Priv *p = (BK_Priv *)plug->priv;
    if (!p) { free(plug); return; }

    KillTimer(p->hwnd, BK_TIMER_ID);
    if (p->hwnd && IsWindow(p->hwnd)) {
        RevokeDragDrop(p->hwnd);
        DestroyWindow(p->hwnd);
    }
    if (p->hbm_mem) DeleteObject(p->hbm_mem);
    if (p->hdc_mem) DeleteDC(p->hdc_mem);
    if (p->font_small) DeleteObject(p->font_small);
    if (p->font_icon)  DeleteObject(p->font_icon);
    DeleteCriticalSection(&p->lock);
    free(p);
    free(plug);
}

void basket_show(IDM_PluginHooks *plug, BOOL visible) {
    if (!plug) return;
    BK_Priv *p = (BK_Priv *)plug->priv;
    if (!p || !p->hwnd) return;
    ShowWindow(p->hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) InvalidateRect(p->hwnd, NULL, FALSE);
}

BOOL basket_visible(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    BK_Priv *p = (BK_Priv *)plug->priv;
    return p && p->hwnd ? IsWindowVisible(p->hwnd) : FALSE;
}

void basket_set_mode(IDM_PluginHooks *plug, BasketMode mode) {
    if (!plug) return;
    BK_Priv *p = (BK_Priv *)plug->priv;
    if (!p || !p->hwnd) return;
    p->mode = mode;
    int nw = (mode == BASKET_MODE_NORMAL) ? BK_NORMAL_W : BK_COMPACT_W;
    int nh = (mode == BASKET_MODE_NORMAL) ? BK_NORMAL_H : BK_COMPACT_H;
    RECT wr; GetWindowRect(p->hwnd, &wr);
    SetWindowPos(p->hwnd, NULL, wr.left, wr.top, nw, nh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(p->hwnd, NULL, TRUE);
}

BasketMode basket_get_mode(IDM_PluginHooks *plug) {
    if (!plug) return BASKET_MODE_NORMAL;
    BK_Priv *p = (BK_Priv *)plug->priv;
    return p ? p->mode : BASKET_MODE_NORMAL;
}

void basket_move_to_corner(IDM_PluginHooks *plug) {
    if (!plug) return;
    BK_Priv *p = (BK_Priv *)plug->priv;
    if (!p || !p->hwnd) return;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int nw = (p->mode == BASKET_MODE_NORMAL) ? BK_NORMAL_W : BK_COMPACT_W;
    int nh = (p->mode == BASKET_MODE_NORMAL) ? BK_NORMAL_H : BK_COMPACT_H;
    SetWindowPos(p->hwnd, HWND_TOPMOST,
                 sw - nw - 16, sh - nh - 48,
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}
