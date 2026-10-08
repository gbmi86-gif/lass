/* plugin_clipboard.c
   ─────────────────────────────────────────────────────────────
   Clipboard Monitor Plugin

   روش فنی:
     - SetClipboardViewer / AddClipboardFormatListener (Win Vista+)
     - یه پنجره مخفی برای دریافت WM_CLIPBOARDUPDATE
     - هر تغییر: متن رو بررسی کن → اگه URL بود callback بزن
     - آخرین URL رو ذخیره می‌کنه تا duplicate جلوگیری کنه

   سازگاری:
     - Vista+ از AddClipboardFormatListener استفاده می‌کنه
     - XP fallback به SetClipboardViewer (chain)
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "plugin_clipboard.h"
#include "../src/lass_log.h"
#include "../src/types.h"

#define CB_WNDCLASS   "IDM_ClipboardWatcher_v1"
#define CB_MAX_EXTS   16
#define CB_EXT_LEN    16

typedef struct {
    HWND            hwnd;           /* پنجره مخفی */
    HWND            owner_hwnd;
    BOOL            enabled;

    CB_UrlDetected  on_url;
    void           *userdata;

    char            last_url[IDM_MAX_URL];  /* جلوگیری از تکرار */
    char            exts[CB_MAX_EXTS][CB_EXT_LEN]; /* فیلتر پسوند */
    int             ext_count;

    /* Vista+ clipboard listener */
    BOOL            using_listener; /* AddClipboardFormatListener */
    HWND            next_viewer;    /* XP chain */

    CRITICAL_SECTION lock;
} CB_Priv;

/* ── forward ── */
static void cb_process_clipboard(CB_Priv *p);

/* ======================================================
   بررسی اینکه URL معتبر و قابل دانلود هست
   ====================================================== */
static BOOL cb_is_url(const char *text) {
    if (!text || strlen(text) < 8) return FALSE;
    /* فقط http/https/ftp */
    if (_strnicmp(text, "http://",  7) != 0 &&
        _strnicmp(text, "https://", 8) != 0 &&
        _strnicmp(text, "ftp://",   6) != 0)
        return FALSE;
    /* نباید newline یا whitespace داشته باشه (URL خالص) */
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') return FALSE;
        if ((unsigned char)*p < 32) return FALSE;
    }
    /* حداقل یه . بعد از scheme داشته باشه */
    const char *after = strchr(text + 8, '.');
    return (after != NULL);
}

static BOOL cb_matches_filter(CB_Priv *p, const char *url) {
    if (p->ext_count == 0) return TRUE; /* بدون فیلتر = همه */
    for (int i = 0; i < p->ext_count; i++) {
        int ulen = (int)strlen(url);
        int elen = (int)strlen(p->exts[i]);
        if (ulen >= elen &&
            _strnicmp(url + ulen - elen, p->exts[i], elen) == 0)
            return TRUE;
        /* query string رو نادیده بگیر: بررسی قبل از ؟ */
        const char *q = strchr(url, '?');
        if (q) {
            int base_len = (int)(q - url);
            if (base_len >= elen &&
                _strnicmp(url + base_len - elen, p->exts[i], elen) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/* ======================================================
   خواندن و پردازش کلیپ‌بورد
   ====================================================== */
static void cb_process_clipboard(CB_Priv *p) {
    if (!p->enabled) return;

    if (!IsClipboardFormatAvailable(CF_TEXT) &&
        !IsClipboardFormatAvailable(CF_UNICODETEXT)) return;

    if (!OpenClipboard(p->hwnd)) return;

    char  url_ansi[IDM_MAX_URL] = {0};

    /* ترجیح Unicode */
    HANDLE hUni = GetClipboardData(CF_UNICODETEXT);
    if (hUni) {
        wchar_t *wtext = (wchar_t *)GlobalLock(hUni);
        if (wtext) {
            /* trim whitespace */
            while (*wtext == L' ' || *wtext == L'\t' ||
                   *wtext == L'\n' || *wtext == L'\r') wtext++;
            WideCharToMultiByte(CP_ACP, 0, wtext, -1,
                                url_ansi, IDM_MAX_URL - 1, NULL, NULL);
            GlobalUnlock(hUni);
        }
    } else {
        HANDLE hText = GetClipboardData(CF_TEXT);
        if (hText) {
            char *text = (char *)GlobalLock(hText);
            if (text) {
                strncpy(url_ansi, text, IDM_MAX_URL - 1);
                GlobalUnlock(hText);
            }
        }
    }

    CloseClipboard();

    /* trim trailing whitespace */
    int len = (int)strlen(url_ansi);
    while (len > 0 && (url_ansi[len-1] == ' ' || url_ansi[len-1] == '\n' ||
                        url_ansi[len-1] == '\r' || url_ansi[len-1] == '\t'))
        url_ansi[--len] = '\0';

    if (!cb_is_url(url_ansi)) return;
    if (!cb_matches_filter(p, url_ansi)) return;

    EnterCriticalSection(&p->lock);
    BOOL is_duplicate = (strcmp(url_ansi, p->last_url) == 0);
    if (!is_duplicate) {
        snprintf(p->last_url, IDM_MAX_URL, "%s", url_ansi);
    }
    LeaveCriticalSection(&p->lock);

    if (is_duplicate) return;

    LOGI(LOG_MOD_CLIPBOARD, "URL detected: %.80s...", url_ansi);

    /* callback به GUI */
    if (p->on_url)
        p->on_url(url_ansi, p->userdata);
}

/* ======================================================
   WndProc پنجره مخفی
   ====================================================== */
static LRESULT CALLBACK cb_wnd_proc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp) {
    CB_Priv *p = (CB_Priv *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        return 0;

    /* Vista+: AddClipboardFormatListener */
    case WM_CLIPBOARDUPDATE:
        if (p) cb_process_clipboard(p);
        return 0;

    /* XP: SetClipboardViewer chain */
    case WM_DRAWCLIPBOARD:
        if (p) {
            cb_process_clipboard(p);
            if (p->next_viewer)
                SendMessageA(p->next_viewer, msg, wp, lp);
        }
        return 0;

    case WM_CHANGECBCHAIN:
        if (p) {
            if ((HWND)wp == p->next_viewer)
                p->next_viewer = (HWND)lp;
            else if (p->next_viewer)
                SendMessageA(p->next_viewer, msg, wp, lp);
        }
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ======================================================
   ثبت class
   ====================================================== */
static BOOL cb_register_class(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = CB_WNDCLASS;
    wc.lpfnWndProc   = cb_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    return (RegisterClassExA(&wc) != 0) ||
           (GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

/* ======================================================
   Plugin hooks (بدون عملکرد خاص — clipboard از پنجره مخفی میاد)
   ====================================================== */
static int  cb_on_init   (void) { return 0; }
static void cb_on_destroy(void) {}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *clipboard_plugin_create(HWND owner_hwnd,
                                          CB_UrlDetected on_url,
                                          void *userdata) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;

    CB_Priv *p = (CB_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->owner_hwnd = owner_hwnd;
    p->on_url     = on_url;
    p->userdata   = userdata;
    p->enabled    = FALSE; /* پیش‌فرض خاموش — کاربر باید فعال کنه */

    /* پنجره مخفی */
    cb_register_class();
    p->hwnd = CreateWindowExA(0, CB_WNDCLASS, "",
                               WS_POPUP, 0, 0, 0, 0,
                               HWND_MESSAGE, /* message-only window */
                               NULL, GetModuleHandleA(NULL), NULL);
    if (!p->hwnd) {
        DeleteCriticalSection(&p->lock);
        free(p); free(plug);
        return NULL;
    }
    SetWindowLongPtrA(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);

    /* ثبت clipboard listener */
    /* AddClipboardFormatListener: Vista+ */
    typedef BOOL (WINAPI *PFN_ACFL)(HWND);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    FARPROC raw_acfl = user32
        ? GetProcAddress(user32, "AddClipboardFormatListener")
        : NULL;
    PFN_ACFL pfn = (PFN_ACFL)(void *)raw_acfl;

    if (pfn && pfn(p->hwnd)) {
        p->using_listener = TRUE;
        LOGI(LOG_MOD_CLIPBOARD, "using AddClipboardFormatListener");
    } else {
        /* XP fallback */
        p->next_viewer = SetClipboardViewer(p->hwnd);
        p->using_listener = FALSE;
        LOGI(LOG_MOD_CLIPBOARD, "using SetClipboardViewer (XP mode)");
    }

    plug->priv        = p;
    plug->name        = "clipboard_monitor";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = cb_on_init;
    plug->on_destroy  = cb_on_destroy;

    LOGI(LOG_MOD_CLIPBOARD, "plugin created");
    return plug;
}

void clipboard_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    CB_Priv *p = (CB_Priv *)plug->priv;
    if (!p) { free(plug); return; }

    /* unregister */
    if (p->hwnd && IsWindow(p->hwnd)) {
        if (p->using_listener) {
            typedef BOOL (WINAPI *PFN_RCFL)(HWND);
            HMODULE user32 = GetModuleHandleA("user32.dll");
            FARPROC raw = user32
                ? GetProcAddress(user32, "RemoveClipboardFormatListener")
                : NULL;
            PFN_RCFL pfn = (PFN_RCFL)(void *)raw;
            if (pfn) pfn(p->hwnd);
        } else {
            ChangeClipboardChain(p->hwnd, p->next_viewer);
        }
        DestroyWindow(p->hwnd);
    }

    DeleteCriticalSection(&p->lock);
    free(p);
    free(plug);
}

void clipboard_plugin_set_enabled(IDM_PluginHooks *plug, BOOL enabled) {
    if (!plug) return;
    CB_Priv *p = (CB_Priv *)plug->priv;
    if (!p) return;
    p->enabled = enabled;
    LOGI(LOG_MOD_CLIPBOARD, "monitor %s", enabled ? "enabled" : "disabled");
}

BOOL clipboard_plugin_is_enabled(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    CB_Priv *p = (CB_Priv *)plug->priv;
    return p ? p->enabled : FALSE;
}

void clipboard_plugin_get_last_url(IDM_PluginHooks *plug,
                                    char *out, int max_len) {
    if (!plug || !out || max_len <= 0) return;
    CB_Priv *p = (CB_Priv *)plug->priv;
    if (!p) { out[0] = '\0'; return; }
    EnterCriticalSection(&p->lock);
    strncpy(out, p->last_url, max_len - 1);
    out[max_len - 1] = '\0';
    LeaveCriticalSection(&p->lock);
}

void clipboard_plugin_set_filter(IDM_PluginHooks *plug, const char *exts) {
    if (!plug) return;
    CB_Priv *p = (CB_Priv *)plug->priv;
    if (!p) return;

    EnterCriticalSection(&p->lock);
    p->ext_count = 0;
    memset(p->exts, 0, sizeof(p->exts));

    if (exts) {
        const char *cur = exts;
        while (*cur && p->ext_count < CB_MAX_EXTS) {
            strncpy(p->exts[p->ext_count], cur, CB_EXT_LEN - 1);
            p->ext_count++;
            cur += strlen(cur) + 1;
        }
    }
    LeaveCriticalSection(&p->lock);

    LOGI(LOG_MOD_CLIPBOARD, "filter set: %d extension(s)", p->ext_count);
}
