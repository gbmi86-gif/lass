/* plugin_basket.h
   ─────────────────────────────────────────────────────────────
   Floating Download Basket – سبد دانلود شناور

   یه پنجره کوچک همیشه‌روی‌بقیه (Always On Top) که:
     - URL می‌پذیره (drag & drop یا paste)
     - آمار کلی نشون میده (تعداد فعال، سرعت کل)
     - دانلود جدید رو مستقیم شروع می‌کنه
     - قابل جابجایی با drag
     - دو حالت: Normal (120×80) و Compact (48×48 دایره)

   استفاده در main.c:
     g_basket = basket_plugin_create(g_hwnd, g_engine, on_basket_drop);
     ...
     basket_show(g_basket, TRUE);
     basket_plugin_destroy(g_basket);
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_BASKET_H
#define IDM_PLUGIN_BASKET_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* callback وقتی URL روی سبد رها شد / paste شد */
typedef void (*Basket_DropCb)(const char *url, void *userdata);

/* حالت نمایش سبد */
typedef enum {
    BASKET_MODE_NORMAL  = 0,   /* 120×80 با متن */
    BASKET_MODE_COMPACT = 1,   /* 48×48 دایره */
} BasketMode;

/* ── ساخت/نابود ─────────────────────────────────────── */
IDM_PluginHooks *basket_plugin_create(HWND         owner_hwnd,
                                       IDM_Engine  *engine,
                                       Basket_DropCb on_drop,
                                       void         *userdata);
void             basket_plugin_destroy(IDM_PluginHooks *plug);

/* ── کنترل نمایش ────────────────────────────────────── */
void        basket_show   (IDM_PluginHooks *plug, BOOL visible);
BOOL        basket_visible(IDM_PluginHooks *plug);
void        basket_set_mode(IDM_PluginHooks *plug, BasketMode mode);
BasketMode  basket_get_mode(IDM_PluginHooks *plug);

/* موقعیت پیش‌فرض: گوشه پایین‌راست صفحه */
void basket_move_to_corner(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_BASKET_H */
