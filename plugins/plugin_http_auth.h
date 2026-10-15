/* plugin_http_auth.h
   ─────────────────────────────────────────────────────────────
   HTTP Authentication Plugin

   پشتیبانی از:
     - Basic Authentication  (RFC 7617)
     - Digest Authentication (RFC 7616)
     - per-domain credential store

   استفاده:
     auth_plugin_add(plug, "example.com", "user", "pass", AUTH_BASIC);
     auth_plugin_apply(plug, hRequest, "example.com");
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_HTTP_AUTH_H
#define IDM_PLUGIN_HTTP_AUTH_H

#include <windows.h>
#include <wininet.h>
#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTH_NONE   = 0,
    AUTH_BASIC  = 1,
    AUTH_DIGEST = 2,
} AuthType;

typedef struct {
    char     domain[256];
    char     user[128];
    char     pass[128];
    AuthType type;
} AuthEntry;

#define AUTH_MAX_ENTRIES 32

/* ── ساخت/نابود ── */
IDM_PluginHooks *auth_plugin_create (void);
void             auth_plugin_destroy(IDM_PluginHooks *plug);

/* ── مدیریت credential ── */
BOOL auth_plugin_add   (IDM_PluginHooks *plug, const char *domain,
                         const char *user, const char *pass, AuthType type);
void auth_plugin_remove(IDM_PluginHooks *plug, const char *domain);
void auth_plugin_clear (IDM_PluginHooks *plug);
int  auth_plugin_count (IDM_PluginHooks *plug);

/* ── پیدا کردن credential برای domain ── */
BOOL auth_plugin_find  (IDM_PluginHooks *plug, const char *domain,
                         AuthEntry *out);

/* ── اعمال روی WinINet request handle ── */
BOOL auth_plugin_apply (IDM_PluginHooks *plug, HINTERNET hReq,
                         const char *domain);

/* ── ساخت Basic Auth header value: "Basic dXNlcjpwYXNz" ── */
void auth_make_basic_header(const char *user, const char *pass,
                             char *out, int out_sz);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_HTTP_AUTH_H */
