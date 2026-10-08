/* plugin_proxy.c
   ─────────────────────────────────────────────────────────────
   Proxy Support Plugin  –  WinINet-based

   روش فنی:
     - HTTP proxy: InternetOpen با INTERNET_OPEN_TYPE_PROXY
     - SOCKS4/5: از WinINet proxy string "socks=host:port" استفاده
     - Basic auth: INTERNET_OPTION_PROXY_USERNAME / PASSWORD
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_proxy.h"
#include "../src/lass_log.h"

typedef struct {
    ProxyConfig config;
    BOOL        active;
    CRITICAL_SECTION lock;
} Proxy_Priv;

/* ======================================================
   parse proxy URL
   ====================================================== */
static BOOL parse_proxy_url(const char *url, ProxyConfig *out) {
    if (!url || !out) return FALSE;
    memset(out, 0, sizeof(*out));

    const char *p = url;

    /* scheme */
    if (_strnicmp(p, "http://", 7) == 0) {
        out->type = PROXY_HTTP; p += 7;
    } else if (_strnicmp(p, "socks5://", 9) == 0) {
        out->type = PROXY_SOCKS5; p += 9;
    } else if (_strnicmp(p, "socks4://", 9) == 0) {
        out->type = PROXY_SOCKS4; p += 9;
    } else if (_strnicmp(p, "socks://", 8) == 0) {
        out->type = PROXY_SOCKS5; p += 8;
    } else {
        /* بدون scheme: فرض HTTP */
        out->type = PROXY_HTTP;
    }

    /* user:pass@ */
    const char *at = strchr(p, '@');
    const char *colon, *host_start;
    if (at) {
        colon = memchr(p, ':', at - p);
        if (colon) {
            size_t ulen = colon - p, plen = at - colon - 1;
            if (ulen < sizeof(out->user))  { memcpy(out->user, p, ulen); }
            if (plen < sizeof(out->pass))  { memcpy(out->pass, colon+1, plen); }
        } else {
            size_t ulen = at - p;
            if (ulen < sizeof(out->user)) memcpy(out->user, p, ulen);
        }
        host_start = at + 1;
    } else {
        host_start = p;
    }

    /* host:port */
    const char *port_colon = strrchr(host_start, ':');
    if (port_colon) {
        size_t hlen = port_colon - host_start;
        if (hlen >= sizeof(out->host)) hlen = sizeof(out->host) - 1;
        memcpy(out->host, host_start, hlen);
        out->port = atoi(port_colon + 1);
    } else {
        snprintf(out->host, sizeof(out->host), "%s", host_start);
        out->port = (out->type == PROXY_HTTP) ? 8080 : 1080;
    }

    return (out->host[0] != '\0' && out->port > 0);
}

/* ======================================================
   Plugin hooks
   ====================================================== */
static int  proxy_on_init   (void) { return 0; }
static void proxy_on_destroy(void) {}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *proxy_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    Proxy_Priv *p = (Proxy_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->active = FALSE;

    plug->priv        = p;
    plug->name        = "proxy_support";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = proxy_on_init;
    plug->on_destroy  = proxy_on_destroy;

    LOGI(LOG_MOD_PROXY, "plugin created");
    return plug;
}

void proxy_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    Proxy_Priv *p = (Proxy_Priv *)plug->priv;
    if (p) {
        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}

BOOL proxy_plugin_set_from_url(IDM_PluginHooks *plug, const char *proxy_url) {
    if (!plug || !proxy_url) return FALSE;
    Proxy_Priv *p = (Proxy_Priv *)plug->priv;
    if (!p) return FALSE;

    ProxyConfig cfg;
    if (!parse_proxy_url(proxy_url, &cfg)) return FALSE;

    EnterCriticalSection(&p->lock);
    p->config = cfg;
    p->active = TRUE;
    LeaveCriticalSection(&p->lock);

    LOGI(LOG_MOD_PROXY, "set: type=%d host=%s port=%d user=%s",
         cfg.type, cfg.host, cfg.port,
         cfg.user[0] ? cfg.user : "(anonymous)");
    return TRUE;
}

void proxy_plugin_clear(IDM_PluginHooks *plug) {
    if (!plug) return;
    Proxy_Priv *p = (Proxy_Priv *)plug->priv;
    if (!p) return;
    EnterCriticalSection(&p->lock);
    memset(&p->config, 0, sizeof(p->config));
    p->active = FALSE;
    LeaveCriticalSection(&p->lock);
    LOGI(LOG_MOD_PROXY, "cleared");
}

BOOL proxy_plugin_is_active(IDM_PluginHooks *plug) {
    if (!plug) return FALSE;
    Proxy_Priv *p = (Proxy_Priv *)plug->priv;
    return p ? p->active : FALSE;
}

void proxy_plugin_get_config(IDM_PluginHooks *plug, ProxyConfig *out) {
    if (!plug || !out) return;
    Proxy_Priv *p = (Proxy_Priv *)plug->priv;
    if (!p) return;
    EnterCriticalSection(&p->lock);
    *out = p->config;
    LeaveCriticalSection(&p->lock);
}

BOOL proxy_plugin_apply(IDM_PluginHooks *plug, HINTERNET hInet) {
    if (!plug || !hInet) return FALSE;
    Proxy_Priv *p = (Proxy_Priv *)plug->priv;
    if (!p || !p->active) {
        /* این حالت طبیعیه (کاربر پروکسی رو فعال نکرده)، ولی چون
           تنها راه فهمیدنش برای کاربر همین لاگه، در سطح DEBUG
           ثبتش می‌کنیم — نه WARN، چون خطا نیست. */
        LOGD(LOG_MOD_PROXY, "apply skipped — proxy not active");
        return FALSE;
    }

    EnterCriticalSection(&p->lock);
    ProxyConfig cfg = p->config;
    LeaveCriticalSection(&p->lock);

    /* ساخت proxy string برای WinINet */
    char proxy_str[512];
    switch (cfg.type) {
    case PROXY_HTTP:
        snprintf(proxy_str, sizeof(proxy_str), "http=%s:%d", cfg.host, cfg.port);
        break;
    case PROXY_SOCKS4:
    case PROXY_SOCKS5:
        snprintf(proxy_str, sizeof(proxy_str), "socks=%s:%d", cfg.host, cfg.port);
        break;
    default:
        LOGW(LOG_MOD_PROXY, "apply failed — unknown proxy type %d", cfg.type);
        return FALSE;
    }

    INTERNET_PROXY_INFO pi;
    pi.dwAccessType    = INTERNET_OPEN_TYPE_PROXY;
    pi.lpszProxy       = proxy_str;
    pi.lpszProxyBypass = "localhost;127.0.0.1;<local>";

    BOOL ok = InternetSetOptionA(hInet, INTERNET_OPTION_PROXY,
                                  &pi, sizeof(pi));

    /* auth اگه داشت */
    if (ok && cfg.user[0]) {
        InternetSetOptionA(hInet, INTERNET_OPTION_PROXY_USERNAME,
                           cfg.user, (DWORD)strlen(cfg.user));
        if (cfg.pass[0])
            InternetSetOptionA(hInet, INTERNET_OPTION_PROXY_PASSWORD,
                               cfg.pass, (DWORD)strlen(cfg.pass));
    }

    /* این تنها جایی‌ست که واقعاً معلوم می‌شه proxy روی یه WinINet
       session واقعی اعمال شده یا نه — قبلاً این تابع اصلاً هیچی
       لاگ نمی‌کرد (نه حتی fprintf)، یعنی هیچ‌وقت از lass.log قابل
       تایید نبود که proxy واقعاً اعمال شده یا خیر. */
    if (ok) {
        LOGI(LOG_MOD_PROXY, "applied to session: %s (bypass=localhost;127.0.0.1)",
             proxy_str);
    } else {
        LOGW(LOG_MOD_PROXY, "InternetSetOptionA(INTERNET_OPTION_PROXY) failed "
             "(err=%lu) for %s", GetLastError(), proxy_str);
    }

    return ok;
}
