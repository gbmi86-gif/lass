/* settings.c
   ─────────────────────────────────────────────────────────────
   Lass – Settings Persistence + Startup

   ذخیره/بارگذاری از %APPDATA%\Lass\lass.ini
   Startup از registry: HKCU\...\Run
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "../plugins/security_fixes.h"
#include <shlobj.h>
#include <shlwapi.h>   /* PathCombineA */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "lass_log.h"
#include "lass_paths.h"
#include "secure_store.h"

/* ======================================================
   helper: خواندن int از INI
   ====================================================== */
static int ini_get_int(const char *ini, const char *sec,
                        const char *key, int def) {
    return (int)GetPrivateProfileIntA(sec, key, def, ini);
}

static void ini_get_str(const char *ini, const char *sec,
                         const char *key, const char *def,
                         char *out, int out_sz) {
    GetPrivateProfileStringA(sec, key, def, out, out_sz, ini);
}

static void ini_set_int(const char *ini, const char *sec,
                         const char *key, int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    WritePrivateProfileStringA(sec, key, buf, ini);
}

static void ini_set_str(const char *ini, const char *sec,
                         const char *key, const char *val) {
    WritePrivateProfileStringA(sec, key, val ? val : "", ini);
}

/* ======================================================
   Registry helpers — HKCU\Software\Lass\Settings
   تنظیمات علاوه بر INI در رجیستری هم ذخیره میشن:
   - INI: قابل حمل (USB، backup، share)
   - Registry: یکپارچگی با Windows، سریع‌تر برای خواندن،
     بازیابی بعد از حذف تصادفی INI
   ====================================================== */
#define LASS_REG_SETTINGS lass_reg_settings_key()

static HKEY reg_open_settings(BOOL write_access) {
    HKEY hKey = NULL;
    DWORD disp;
    if (write_access) {
        RegCreateKeyExA(HKEY_CURRENT_USER, LASS_REG_SETTINGS,
                        0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, &disp);
    } else {
        RegOpenKeyExA(HKEY_CURRENT_USER, LASS_REG_SETTINGS,
                      0, KEY_QUERY_VALUE, &hKey);
    }
    return hKey;
}

static void reg_set_int(HKEY hk, const char *name, DWORD val) {
    RegSetValueExA(hk, name, 0, REG_DWORD, (BYTE *)&val, sizeof(val));
}

static void reg_set_str(HKEY hk, const char *name, const char *val) {
    if (!val) val = "";
    RegSetValueExA(hk, name, 0, REG_SZ,
                   (const BYTE *)val, (DWORD)(strlen(val) + 1));
}

static DWORD reg_get_int(HKEY hk, const char *name, DWORD def) {
    DWORD val = def, sz = sizeof(val), type = REG_DWORD;
    RegQueryValueExA(hk, name, NULL, &type, (BYTE *)&val, &sz);
    return val;
}

static void reg_get_str(HKEY hk, const char *name,
                         char *out, int out_sz, const char *def) {
    DWORD sz = (DWORD)out_sz, type = REG_SZ;
    if (RegQueryValueExA(hk, name, NULL, &type,
                          (BYTE *)out, &sz) != ERROR_SUCCESS) {
        if (def && def != out) safe_strncpy(out, def, (size_t)out_sz);
    }
    out[out_sz - 1] = '\0';
}

static void reg_save_all(HKEY hk, const LassSettings *s) {
    reg_set_int(hk, "max_concurrent",   s->max_concurrent);
    reg_set_int(hk, "default_segments", s->default_segments);
    reg_set_int(hk, "minimize_to_tray", s->minimize_to_tray ? 1 : 0);
    reg_set_int(hk, "start_minimized",  s->start_minimized  ? 1 : 0);
    reg_set_str(hk, "default_dir",      s->default_dir);
    reg_set_int(hk, "clipboard_monitor",s->clipboard_monitor ? 1 : 0);
    reg_set_int(hk, "basket_visible",   s->basket_visible    ? 1 : 0);
    reg_set_int(hk, "proxy_enabled",    s->proxy_enabled     ? 1 : 0);
    {
        char enc[1024];
        if (secure_store_encrypt(s->proxy_url, enc, sizeof(enc)))
            reg_set_str(hk, "proxy_url", enc);
    }
    reg_set_int(hk, "win_pos_saved",    s->win_pos_saved ? 1 : 0);
    reg_set_int(hk, "win_x",            (DWORD)s->win_x);
    reg_set_int(hk, "win_y",            (DWORD)s->win_y);
    reg_set_int(hk, "win_w",            (DWORD)s->win_w);
    reg_set_int(hk, "win_h",            (DWORD)s->win_h);
    reg_set_int(hk, "run_on_startup",   s->run_on_startup ? 1 : 0);
    reg_set_str(hk, "language",         s->language);
}

static void reg_load_all(HKEY hk, LassSettings *s) {
    s->max_concurrent   = reg_get_int(hk, "max_concurrent",   s->max_concurrent);
    s->default_segments = reg_get_int(hk, "default_segments", s->default_segments);
    s->minimize_to_tray = reg_get_int(hk, "minimize_to_tray", s->minimize_to_tray);
    s->start_minimized  = reg_get_int(hk, "start_minimized",  s->start_minimized);
    reg_get_str(hk, "default_dir", s->default_dir,
                sizeof(s->default_dir), s->default_dir);
    s->clipboard_monitor = reg_get_int(hk, "clipboard_monitor", s->clipboard_monitor);
    s->basket_visible    = reg_get_int(hk, "basket_visible",    s->basket_visible);
    s->proxy_enabled     = reg_get_int(hk, "proxy_enabled",     s->proxy_enabled);
    {
        char raw[1024] = {0};
        reg_get_str(hk, "proxy_url", raw, sizeof(raw), "");
        if (!secure_store_decrypt(raw, s->proxy_url, sizeof(s->proxy_url)))
            s->proxy_url[0] = '\0';
    }
    s->win_pos_saved = reg_get_int(hk, "win_pos_saved", s->win_pos_saved);
    s->win_x = (int)reg_get_int(hk, "win_x", (DWORD)s->win_x);
    s->win_y = (int)reg_get_int(hk, "win_y", (DWORD)s->win_y);
    s->win_w = (int)reg_get_int(hk, "win_w", (DWORD)s->win_w);
    s->win_h = (int)reg_get_int(hk, "win_h", (DWORD)s->win_h);
    s->run_on_startup = reg_get_int(hk, "run_on_startup", s->run_on_startup);
    {
        char lang_raw[8];
        reg_get_str(hk, "language", lang_raw, sizeof(lang_raw), s->language);
        strncpy(s->language, lang_raw, sizeof(s->language) - 1);
        s->language[sizeof(s->language) - 1] = '\0';
    }
}

/* ======================================================
   مسیر INI
   ====================================================== */
void settings_get_ini_path(char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = '\0';

    /* پوشه‌ی داده از lass_paths: %APPDATA%\Lass، یا LASS_DATA_DIR در حالت تست */
    char dir[MAX_PATH * 2] = {0};
    if (!lass_data_dir(dir, sizeof(dir))) {
        /* آخرین راه‌حل: پوشه‌ی موقت، مثل قبل */
        char tmp[MAX_PATH] = {0};
        GetTempPathA(sizeof(tmp), tmp);
        PathCombineA(dir, tmp, LASS_APP_NAME);
    }
    CreateDirectoryA(dir, NULL);

    char full[MAX_PATH * 2] = {0};
    PathCombineA(full, dir, LASS_INI_NAME);
    strncpy(out, full, out_sz - 1);
    out[out_sz - 1] = '\0';
}

/* ======================================================
   مقادیر پیش‌فرض
   ====================================================== */
void settings_defaults(LassSettings *s) {
    if (!s) return;
    s->max_concurrent   = 5;
    s->default_segments = 8;
    s->minimize_to_tray = TRUE;
    s->start_minimized  = FALSE;
    s->run_on_startup   = FALSE;
    s->clipboard_monitor = FALSE;
    s->basket_visible   = FALSE;
    s->proxy_enabled    = FALSE;
    s->win_x = CW_USEDEFAULT;
    s->win_y = CW_USEDEFAULT;
    s->win_w = 820;
    s->win_h = 540;
    s->win_pos_saved = FALSE;
    strncpy(s->language, "en", sizeof(s->language) - 1);
    s->language[sizeof(s->language) - 1] = '\0';

    /* default_dir = Downloads folder با PathCombineA */
    char docs[MAX_PATH] = {0};
    char downloads[MAX_PATH * 2] = {0};
    if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs) == S_OK) {
        PathCombineA(downloads, docs, "Downloads");
        strncpy(s->default_dir, downloads, sizeof(s->default_dir) - 1);
        s->default_dir[sizeof(s->default_dir) - 1] = '\0';
    } else {
        GetTempPathA(sizeof(s->default_dir), s->default_dir);
    }
    CreateDirectoryA(s->default_dir, NULL);
}

/* ======================================================
   بارگذاری
   ====================================================== */
void settings_load(LassSettings *s) {
    if (!s) return;
    settings_defaults(s);

    char ini[MAX_PATH];
    settings_get_ini_path(ini, sizeof(ini));

    /* اگه فایل نبود از رجیستری بخون (fallback) */
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        HKEY hk = reg_open_settings(FALSE);
        if (hk) {
            LOGI(LOG_MOD_SETTINGS, "INI not found, loading from Registry");
            reg_load_all(hk, s);
            RegCloseKey(hk);
            /* clamp */
            if (s->max_concurrent < 1)    s->max_concurrent = 1;
            if (s->max_concurrent > 20)   s->max_concurrent = 20;
            if (s->default_segments < 1)  s->default_segments = 1;
            if (s->default_segments > 32) s->default_segments = 32;
        } else {
            LOGI(LOG_MOD_SETTINGS, "No INI or Registry found, using defaults: %s", ini);
        }
        return;
    }

    /* [General] */
    s->max_concurrent   = (uint32_t)ini_get_int(ini, "General", "max_concurrent",   5);
    s->default_segments = (uint32_t)ini_get_int(ini, "General", "default_segments", 8);
    s->minimize_to_tray = ini_get_int(ini, "General", "minimize_to_tray", 1);
    s->start_minimized  = ini_get_int(ini, "General", "start_minimized",  0);
    {
        char lang_raw[8];
        ini_get_str(ini, "General", "language", "en", lang_raw, sizeof(lang_raw));
        strncpy(s->language, lang_raw, sizeof(s->language) - 1);
        s->language[sizeof(s->language) - 1] = '\0';
    }

    char def_dir[MAX_PATH];
    snprintf(def_dir, sizeof(def_dir), "%s", s->default_dir);
    ini_get_str(ini, "General", "default_dir", def_dir,
                s->default_dir, sizeof(s->default_dir));

    /* clamp */
    if (s->max_concurrent < 1)   s->max_concurrent = 1;
    if (s->max_concurrent > 20)  s->max_concurrent = 20;
    if (s->default_segments < 1) s->default_segments = 1;
    if (s->default_segments > 32) s->default_segments = 32;

    /* [Plugins] */
    s->clipboard_monitor = ini_get_int(ini, "Plugins", "clipboard_monitor", 0);
    s->basket_visible    = ini_get_int(ini, "Plugins", "basket_visible",    0);
    s->proxy_enabled     = ini_get_int(ini, "Plugins", "proxy_enabled",     0);
    {
        char raw[1024] = {0};
        ini_get_str(ini, "Plugins", "proxy_url", "", raw, sizeof(raw));
        if (!secure_store_decrypt(raw, s->proxy_url, sizeof(s->proxy_url))) {
            /* decrypt fail شد (مثلاً فایل از یه کاربر/ماشین دیگه
               کپی شده) — امن‌ترین کار خالی گذاشتنشه، نه استفاده
               از داده‌ی نامعتبر یا نیمه‌decrypt شده */
            s->proxy_url[0] = '\0';
            LOGW(LOG_MOD_SETTINGS,
                 "proxy_url could not be decrypted — reset to empty "
                 "(normal if config copied from another user/machine)");
        }
    }

    /* [Window] */
    s->win_pos_saved = ini_get_int(ini, "Window", "saved", 0);
    if (s->win_pos_saved) {
        s->win_x = ini_get_int(ini, "Window", "x", CW_USEDEFAULT);
        s->win_y = ini_get_int(ini, "Window", "y", CW_USEDEFAULT);
        s->win_w = ini_get_int(ini, "Window", "w", 820);
        s->win_h = ini_get_int(ini, "Window", "h", 540);
        /* clamp */
        if (s->win_w < 400) s->win_w = 400;
        if (s->win_h < 300) s->win_h = 300;
        /* بررسی اینکه پنجره داخل صفحه باشه */
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (s->win_x < 0 || s->win_x > sw - 100) s->win_x = 100;
        if (s->win_y < 0 || s->win_y > sh - 100) s->win_y = 100;
    }

    /* [Startup] */
    s->run_on_startup = ini_get_int(ini, "Startup", "run_on_startup", 0);

    LOGI(LOG_MOD_SETTINGS, "Loaded: %s", ini);
}

/* ======================================================
   ذخیره
   ====================================================== */
BOOL settings_save(const LassSettings *s) {
    if (!s) return FALSE;
    char ini[MAX_PATH];
    settings_get_ini_path(ini, sizeof(ini));

    /* [General] */
    ini_set_int(ini, "General", "max_concurrent",   (int)s->max_concurrent);
    ini_set_int(ini, "General", "default_segments", (int)s->default_segments);
    ini_set_int(ini, "General", "minimize_to_tray", s->minimize_to_tray ? 1 : 0);
    ini_set_int(ini, "General", "start_minimized",  s->start_minimized  ? 1 : 0);
    ini_set_str(ini, "General", "default_dir",      s->default_dir);
    ini_set_str(ini, "General", "language",         s->language);

    /* [Plugins] */
    ini_set_int(ini, "Plugins", "clipboard_monitor", s->clipboard_monitor ? 1 : 0);
    ini_set_int(ini, "Plugins", "basket_visible",    s->basket_visible    ? 1 : 0);
    ini_set_int(ini, "Plugins", "proxy_enabled",     s->proxy_enabled     ? 1 : 0);
    {
        /* proxy_url می‌تونه credential داشته باشه (user:pass@host).
           با DPAPI رمزنگاری می‌کنیم تا در فایل INI plaintext نمونه —
           فقط همین user account روی همین ماشین می‌تونه بعداً
           decrypt کنه. */
        char enc[1024];
        if (secure_store_encrypt(s->proxy_url, enc, sizeof(enc))) {
            ini_set_str(ini, "Plugins", "proxy_url", enc);
        } else {
            LOGW(LOG_MOD_SETTINGS,
                 "proxy_url encryption failed — not saving to avoid plaintext leak");
        }
    }

    /* [Window] */
    ini_set_int(ini, "Window", "saved", 1);
    ini_set_int(ini, "Window", "x", s->win_x);
    ini_set_int(ini, "Window", "y", s->win_y);
    ini_set_int(ini, "Window", "w", s->win_w);
    ini_set_int(ini, "Window", "h", s->win_h);

    /* [Startup] */
    ini_set_int(ini, "Startup", "run_on_startup", s->run_on_startup ? 1 : 0);

    LOGI(LOG_MOD_SETTINGS, "Saved: %s", ini);

    /* همزمان در رجیستری هم ذخیره کن — backup و یکپارچگی با Windows */
    HKEY hk = reg_open_settings(TRUE);
    if (hk) {
        reg_save_all(hk, s);
        RegCloseKey(hk);
        LOGI(LOG_MOD_SETTINGS, "Registry sync: HKCU\\%s",
                LASS_REG_SETTINGS);
    }

    return TRUE;
}

/* ======================================================
   Startup Registry
   ====================================================== */
BOOL settings_set_startup(BOOL enable) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, LASS_REG_RUN,
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;

    BOOL ok;
    if (enable) {
        char raw_path[MAX_PATH];
        GetModuleFileNameA(NULL, raw_path, MAX_PATH);

        /* exe_path رو با quotes بپیچون تا اگه مسیر space داشت
           (مثل "C:\Program Files\Lass\Lass.exe") درست parse بشه */
        char exe_path[MAX_PATH + 32];
        snprintf(exe_path, sizeof(exe_path), "\"%s\" /minimized", raw_path);

        ok = (RegSetValueExA(hKey, LASS_REG_KEY, 0, REG_SZ,
                              (const BYTE *)exe_path,
                              (DWORD)(strlen(exe_path) + 1))
              == ERROR_SUCCESS);
    } else {
        LONG r = RegDeleteValueA(hKey, LASS_REG_KEY);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(hKey);
    LOGI(LOG_MOD_SETTINGS, "Startup %s: %s",
            enable ? "enabled" : "disabled", ok ? "OK" : "FAILED");
    return ok;
}

BOOL settings_get_startup(void) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, LASS_REG_RUN,
                      0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    DWORD type, sz = 0;
    BOOL exists = (RegQueryValueExA(hKey, LASS_REG_KEY,
                                     NULL, &type, NULL, &sz)
                   == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

/* ======================================================
   Window Position
   ====================================================== */
void settings_apply_window(const LassSettings *s, HWND hwnd) {
    if (!hwnd || !s->win_pos_saved) return;
    SetWindowPos(hwnd, NULL,
                 s->win_x, s->win_y,
                 s->win_w, s->win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void settings_save_window(LassSettings *s, HWND hwnd) {
    if (!hwnd || !s) return;
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    /* فقط اگه normal بود — نه minimized */
    if (wp.showCmd == SW_SHOWNORMAL || wp.showCmd == SW_SHOW) {
        RECT r = wp.rcNormalPosition;
        s->win_x = r.left;
        s->win_y = r.top;
        s->win_w = r.right  - r.left;
        s->win_h = r.bottom - r.top;
        s->win_pos_saved = TRUE;
    }
}
