/* guo.h
   ─────────────────────────────────────────────────────────────
   Lass – Graphic Unit Operator (UI Layer)

   همه چیز مربوط به نمایش اینجاست:
     - WndProc اصلی
     - Dialogs (add, settings, batch, schedule)
     - ListView
     - Toolbar / Menubar
     - System Tray
     - Speed Graph window

   GUO فقط از core_ldm.h استفاده می‌کنه — نه engine.h مستقیم
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_GUO_H
#define LASS_GUO_H

#include <windows.h>
#include "../core/core_ldm.h"
#include "../src/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================
   GUO context — همه state مربوط به UI
   ====================================================== */
typedef struct {
    /* handles */
    HWND        main_wnd;
    HWND        listview;
    HWND        statusbar;
    HWND        toolbar;
    HINSTANCE   hinstance;

    /* core */
    LDM_Core   *core;
    LassSettings *cfg;

    /* tray */
    BOOL        tray_added;

    /* dialogs باز */
    HWND        add_hwnd;
    HWND        settings_hwnd;
    HWND        batch_hwnd;     /* دیالوگ Batch — برای IsDialogMessage/جلوگیری از چندبار باز شدن */

    /* category filter */
    HWND        cat_combo;     /* combo box فیلتر دسته‌بندی */
    int         cat_filter;    /* -2=All, -1=Other, 0+=index category */

} GUO_Context;

/* ── راه‌اندازی / خاموش کردن ── */
BOOL        guo_init   (GUO_Context *g, HINSTANCE hInst, LDM_Core *core,
                         LassSettings *cfg);
void        guo_destroy(GUO_Context *g);

/* ── message loop ── */
int         guo_run    (GUO_Context *g); /* برمیگردونه exit code */

/* ── نمایش از بیرون ── */
void        guo_show          (GUO_Context *g, BOOL visible);
void        guo_show_add_dialog(GUO_Context *g);
void        guo_show_settings (GUO_Context *g);
void        guo_show_schedule (GUO_Context *g, int dl_id);

#ifdef __cplusplus
}
#endif

#endif /* LASS_GUO_H */
