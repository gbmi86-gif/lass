/* plugin_http_auth.c
   ─────────────────────────────────────────────────────────────
   HTTP Authentication Plugin

   Basic Auth: base64(user:pass) رو در Authorization header میذاره
   Digest Auth: WinINet خودش handle می‌کنه با INTERNET_OPTION_USERNAME
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

#include "plugin_http_auth.h"
#include "../src/lass_log.h"

/* ── Base64 table ── */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, int in_len,
                           char *out, int out_sz) {
    int i = 0, o = 0;
    while (i < in_len && o + 4 < out_sz) {
        unsigned int b = (unsigned int)in[i] << 16;
        if (i+1 < in_len) b |= (unsigned int)in[i+1] << 8;
        if (i+2 < in_len) b |= in[i+2];
        out[o++] = B64[(b >> 18) & 0x3F];
        out[o++] = B64[(b >> 12) & 0x3F];
        out[o++] = (i+1 < in_len) ? B64[(b >> 6) & 0x3F] : '=';
        out[o++] = (i+2 < in_len) ? B64[b & 0x3F] : '=';
        i += 3;
    }
    if (o < out_sz) out[o] = '\0';
}

typedef struct {
    AuthEntry entries[AUTH_MAX_ENTRIES];
    int       count;
    CRITICAL_SECTION lock;
} Auth_Priv;

/* ── plugin hooks ── */
static int  auth_on_init   (void) { return 0; }
static void auth_on_destroy(void) {}

/* همون extract_domain که در plugin_cookie_jar.c هست — عمداً کپی
   شده (نه shared) چون یه تابع خیلی کوچیکه و نمی‌خوایم برای همین
   یه header مشترک جدید اضافه کنیم. */
static void auth_extract_domain(const char *url, char *out, int max) {
    out[0] = '\0';
    if (!url) return;
    const char *p = url;
    if (strncmp(p, "http://",  7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* ── on_http_headers ──
   دقیقاً همون مکانیزم عمومی‌ای که برای Cookie Jar وصل شده
   (plugin_collect_http_headers → http_set_header_provider در
   http.c) — پس با اضافه کردن این hook، بدون هیچ تغییری در
   http.c/engine.c، درخواست‌ها به‌صورت خودکار (preemptive) هدر
   Authorization: Basic می‌گیرن اگه credential ای برای همون domain
   ثبت شده باشه.

   فقط Basic پوشش داده میشه، نه Digest — چون Digest ذاتاً به یه
   چالش قبلی (WWW-Authenticate با nonce از یه پاسخ ۴۰۱) نیاز داره؛
   نمیشه preemptive فرستادش. اضافه کردن Digest واقعی نیاز به
   دست‌کاری جریان اصلی http.c (گرفتن ۴۰۱، parse کردن nonce، retry)
   داره که یه تغییر جدا و بزرگ‌تره. */
static int auth_on_http_headers(IDM_Download *dl, char *buf, int max_len) {
    extern IDM_PluginHooks *g_auth_plugin_instance;
    IDM_PluginHooks *self = g_auth_plugin_instance;
    if (!self || !dl || !buf || max_len <= 0) return 0;

    char domain[256];
    auth_extract_domain(dl->url, domain, sizeof(domain));
    if (!domain[0]) return 0;

    AuthEntry entry;
    if (!auth_plugin_find(self, domain, &entry)) return 0;
    if (entry.type != AUTH_BASIC) return 0; /* Digest اینجا پوشش داده نمیشه */

    char b64[512];
    auth_make_basic_header(entry.user, entry.pass, b64, sizeof(b64));
    int n = snprintf(buf, max_len, "Authorization: %s\r\n", b64);
    return (n > 0 && n < max_len) ? n : 0;
}

IDM_PluginHooks *g_auth_plugin_instance = NULL;

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *auth_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    Auth_Priv *p = (Auth_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);

    plug->priv        = p;
    plug->name        = "http_auth";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = auth_on_init;
    plug->on_destroy  = auth_on_destroy;
    plug->on_http_headers = auth_on_http_headers;

    g_auth_plugin_instance = plug;

    LOGI(LOG_MOD_AUTH, "plugin created");
    return plug;
}

void auth_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    Auth_Priv *p = (Auth_Priv *)plug->priv;
    if (p) { DeleteCriticalSection(&p->lock); free(p); }
    if (g_auth_plugin_instance == plug) g_auth_plugin_instance = NULL;
    free(plug);
}

BOOL auth_plugin_add(IDM_PluginHooks *plug, const char *domain,
                      const char *user, const char *pass, AuthType type) {
    if (!plug || !domain || !user) return FALSE;
    Auth_Priv *p = (Auth_Priv *)plug->priv;
    if (!p) return FALSE;

    EnterCriticalSection(&p->lock);

    /* آپدیت اگه domain قبلاً وجود داشت */
    for (int i = 0; i < p->count; i++) {
        if (_stricmp(p->entries[i].domain, domain) == 0) {
            snprintf(p->entries[i].user, sizeof(p->entries[i].user), "%s", user);
            snprintf(p->entries[i].pass, sizeof(p->entries[i].pass), "%s", pass ? pass : "");
            p->entries[i].type = type;
            LeaveCriticalSection(&p->lock);
            LOGI(LOG_MOD_AUTH, "updated: %s @ %s", user, domain);
            return TRUE;
        }
    }

    /* اضافه جدید */
    if (p->count >= AUTH_MAX_ENTRIES) {
        LeaveCriticalSection(&p->lock);
        return FALSE;
    }

    AuthEntry *e = &p->entries[p->count++];
    snprintf(e->domain, sizeof(e->domain), "%s", domain);
    snprintf(e->user,   sizeof(e->user),   "%s", user);
    snprintf(e->pass,   sizeof(e->pass),   "%s", pass ? pass : "");
    e->type = type;

    LeaveCriticalSection(&p->lock);
    LOGI(LOG_MOD_AUTH, "added: %s @ %s (type=%d)", user, domain, type);
    return TRUE;
}

void auth_plugin_remove(IDM_PluginHooks *plug, const char *domain) {
    if (!plug || !domain) return;
    Auth_Priv *p = (Auth_Priv *)plug->priv;
    if (!p) return;

    EnterCriticalSection(&p->lock);
    for (int i = 0; i < p->count; i++) {
        if (_stricmp(p->entries[i].domain, domain) == 0) {
            /* جا بقیه رو پر کن */
            memmove(&p->entries[i], &p->entries[i+1],
                    (p->count - i - 1) * sizeof(AuthEntry));
            p->count--;
            break;
        }
    }
    LeaveCriticalSection(&p->lock);
}

void auth_plugin_clear(IDM_PluginHooks *plug) {
    if (!plug) return;
    Auth_Priv *p = (Auth_Priv *)plug->priv;
    if (!p) return;
    EnterCriticalSection(&p->lock);
    p->count = 0;
    memset(p->entries, 0, sizeof(p->entries));
    LeaveCriticalSection(&p->lock);
}

int auth_plugin_count(IDM_PluginHooks *plug) {
    if (!plug) return 0;
    Auth_Priv *p = (Auth_Priv *)plug->priv;
    if (!p) return 0;
    EnterCriticalSection(&p->lock);
    int c = p->count;
    LeaveCriticalSection(&p->lock);
    return c;
}

BOOL auth_plugin_find(IDM_PluginHooks *plug, const char *domain,
                       AuthEntry *out) {
    if (!plug || !domain || !out) return FALSE;
    Auth_Priv *p = (Auth_Priv *)plug->priv;
    if (!p) return FALSE;

    EnterCriticalSection(&p->lock);
    BOOL found = FALSE;
    for (int i = 0; i < p->count; i++) {
        /* exact match یا suffix: "sub.example.com" با "example.com" match میشه */
        const char *ed = p->entries[i].domain;
        if (_stricmp(domain, ed) == 0) {
            *out = p->entries[i]; found = TRUE; break;
        }
        /* suffix check */
        int dlen = (int)strlen(domain), elen = (int)strlen(ed);
        if (dlen > elen && domain[dlen - elen - 1] == '.' &&
            _stricmp(domain + dlen - elen, ed) == 0) {
            *out = p->entries[i]; found = TRUE; break;
        }
    }
    LeaveCriticalSection(&p->lock);
    return found;
}

void auth_make_basic_header(const char *user, const char *pass,
                              char *out, int out_sz) {
    if (!user || !out || out_sz < 10) return;
    char creds[300];
    snprintf(creds, sizeof(creds), "%s:%s", user, pass ? pass : "");
    char b64[400];
    base64_encode((const unsigned char *)creds, (int)strlen(creds),
                   b64, sizeof(b64));
    snprintf(out, out_sz, "Basic %s", b64);
}

BOOL auth_plugin_apply(IDM_PluginHooks *plug, HINTERNET hReq,
                        const char *domain) {
    if (!plug || !hReq || !domain) return FALSE;

    AuthEntry e;
    if (!auth_plugin_find(plug, domain, &e)) return FALSE;

    if (e.type == AUTH_BASIC) {
        char hdr_val[512];
        auth_make_basic_header(e.user, e.pass, hdr_val, sizeof(hdr_val));
        char hdr[560];
        snprintf(hdr, sizeof(hdr), "Authorization: %s\r\n", hdr_val);
        HttpAddRequestHeadersA(hReq, hdr, -1L,
                               HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
        return TRUE;
    }

    if (e.type == AUTH_DIGEST) {
        /* WinINet با INTERNET_OPTION_USERNAME/PASSWORD خودش digest رو handle می‌کنه */
        InternetSetOptionA(hReq, INTERNET_OPTION_USERNAME,
                           e.user, (DWORD)strlen(e.user));
        InternetSetOptionA(hReq, INTERNET_OPTION_PASSWORD,
                           e.pass, (DWORD)strlen(e.pass));
        return TRUE;
    }

    return FALSE;
}
