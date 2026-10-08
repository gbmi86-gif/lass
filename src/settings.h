/* settings.h
   ─────────────────────────────────────────────────────────────
   Lass – Settings Persistence

   فایل: %APPDATA%\Lass\lass.ini
   فرمت: INI استاندارد

   [General]
   max_concurrent=5
   default_segments=8
   minimize_to_tray=1
   default_dir=C:\Downloads

   [Plugins]
   clipboard_monitor=0
   basket_visible=0
   proxy_url=
   proxy_enabled=0

   [Window]
   x=100
   y=100
   w=820
   h=540

   [Startup]
   run_on_startup=0
   start_minimized=0
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_SETTINGS_H
#define LASS_SETTINGS_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#define LASS_APP_NAME    "Lass"
#define LASS_INI_NAME    "lass.ini"
#define LASS_REG_RUN     "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define LASS_REG_KEY     "Lass Download Manager"

typedef struct {
    /* [General] */
    uint32_t max_concurrent;
    uint32_t default_segments;
    BOOL     minimize_to_tray;
    BOOL     start_minimized;
    char     default_dir[MAX_PATH];

    /* [Plugins] */
    BOOL     clipboard_monitor;
    BOOL     basket_visible;
    BOOL     proxy_enabled;
    char     proxy_url[512];

    /* [Window] */
    int      win_x, win_y;
    int      win_w, win_h;
    BOOL     win_pos_saved;

    /* [Startup] */
    BOOL     run_on_startup;

    /* [General] — زبان UI: "en" یا "fa". اضافه‌شده در فاز اول i18n؛
       اگه مقدار در ini نبود یا ناشناخته بود، همیشه به "en" برمی‌گرده
       (i18n_lang_from_code تضمین می‌کنه). */
    char     language[8];
} LassSettings;

/* ── پیدا کردن مسیر INI: %APPDATA%\Lass\lass.ini ── */
void settings_get_ini_path(char *out, int out_sz);

/* ── بارگذاری — اگه فایل نبود مقادیر پیش‌فرض ── */
void settings_load(LassSettings *s);

/* ── ذخیره ── */
BOOL settings_save(const LassSettings *s);

/* ── Startup registry ── */
BOOL settings_set_startup(BOOL enable);
BOOL settings_get_startup(void);

/* ── اعمال window position ── */
void settings_apply_window(const LassSettings *s, HWND hwnd);
void settings_save_window (LassSettings *s, HWND hwnd);

/* ── مقادیر پیش‌فرض ── */
void settings_defaults(LassSettings *s);

#endif /* LASS_SETTINGS_H */
