/* plugin_proxy.h
   ─────────────────────────────────────────────────────────────
   Proxy Support Plugin

   پشتیبانی از:
     - HTTP proxy  (CONNECT tunneling)
     - SOCKS4 / SOCKS5
     - Basic proxy authentication

   استفاده:
     proxy_plugin_set(plug, "http://user:pass@proxy.host:8080");
     proxy_plugin_set(plug, "socks5://127.0.0.1:1080");
     proxy_plugin_clear(plug);  // غیرفعال
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_PROXY_H
#define IDM_PLUGIN_PROXY_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROXY_NONE   = 0,
    PROXY_HTTP   = 1,
    PROXY_SOCKS4 = 2,
    PROXY_SOCKS5 = 3,
} ProxyType;

typedef struct {
    ProxyType type;
    char      host[256];
    int       port;
    char      user[128];
    char      pass[128];
} ProxyConfig;

/* ── ساخت/نابود ── */
IDM_PluginHooks *proxy_plugin_create(void);
void             proxy_plugin_destroy(IDM_PluginHooks *plug);

/* ── تنظیم از URL: "http://user:pass@host:port" یا "socks5://host:port" ── */
BOOL proxy_plugin_set_from_url(IDM_PluginHooks *plug, const char *proxy_url);
void proxy_plugin_clear       (IDM_PluginHooks *plug);
BOOL proxy_plugin_is_active   (IDM_PluginHooks *plug);
void proxy_plugin_get_config  (IDM_PluginHooks *plug, ProxyConfig *out);

/* ── اعمال روی WinINet HINTERNET handle ── */
BOOL proxy_plugin_apply(IDM_PluginHooks *plug, HINTERNET hInet);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_PROXY_H */
