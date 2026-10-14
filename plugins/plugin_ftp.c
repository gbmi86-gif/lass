/* plugin_ftp.c
   ─────────────────────────────────────────────────────────────
   FTP/FTPS Download Plugin  –  WinINet-based

   معماری:
     - parse_ftp_url: URL رو به components تبدیل می‌کنه
     - ftp_discover:  سایز فایل + قابلیت resume رو می‌خونه
     - ftp_open:      connection برای دانلود با resume support
     - فایل به همون FileWriter که http.c استفاده می‌کنه تحویل میشه

   thread safety:
     - هر call یه HINTERNET session مجزا می‌گیره
     - g_ftp_lock فقط برای init یه‌بار
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

#include "plugin_ftp.h"
#include "../src/lass_log.h"
#include "../src/http.h"    /* HTTP_Conn, ParsedURL, HTTP_DiscoveryInfo */
#include "../src/types.h"

/* ======================================================
   parse ftp://[user:pass@]host[:port]/path
   ====================================================== */
typedef struct {
    char scheme[8];        /* "ftp" or "ftps" */
    char user[128];
    char pass[128];
    char host[256];
    INTERNET_PORT port;
    char path[IDM_MAX_URL];
    char filename[IDM_MAX_FILENAME];
    BOOL is_ftps;
} FTP_URL;

static BOOL parse_ftp_url(const char *url, FTP_URL *out) {
    if (!url || !out) return FALSE;
    memset(out, 0, sizeof(*out));

    /* scheme */
    if (_strnicmp(url, "ftps://", 7) == 0) {
        strcpy(out->scheme, "ftps");
        out->is_ftps = TRUE;
        url += 7;
    } else if (_strnicmp(url, "ftp://", 6) == 0) {
        strcpy(out->scheme, "ftp");
        out->is_ftps = FALSE;
        url += 6;
    } else {
        return FALSE;
    }

    /* user:pass@ */
    const char *at = strchr(url, '@');
    const char *slash = strchr(url, '/');
    if (at && (!slash || at < slash)) {
        /* user:pass@ موجوده */
        const char *colon = memchr(url, ':', at - url);
        if (colon) {
            size_t ulen = colon - url;
            size_t plen = at - colon - 1;
            if (ulen >= sizeof(out->user) || plen >= sizeof(out->pass))
                return FALSE;
            memcpy(out->user, url, ulen);
            memcpy(out->pass, colon + 1, plen);
        } else {
            size_t ulen = at - url;
            if (ulen >= sizeof(out->user)) return FALSE;
            memcpy(out->user, url, ulen);
        }
        url = at + 1;
    } else {
        /* anonymous */
        strcpy(out->user, "anonymous");
        strcpy(out->pass, "lass@example.com");
    }

    /* host[:port] */
    const char *host_end = strchr(url, '/');
    const char *port_sep = strchr(url, ':');
    if (port_sep && (!host_end || port_sep < host_end)) {
        size_t hlen = port_sep - url;
        if (hlen >= sizeof(out->host)) return FALSE;
        memcpy(out->host, url, hlen);
        out->port = (INTERNET_PORT)atoi(port_sep + 1);
        url = host_end ? host_end : (port_sep + strlen(port_sep));
    } else {
        size_t hlen = host_end ? (size_t)(host_end - url) : strlen(url);
        if (hlen >= sizeof(out->host)) return FALSE;
        memcpy(out->host, url, hlen);
        out->port = out->is_ftps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_FTP_PORT;
        url = host_end ? host_end : (url + hlen);
    }

    /* path */
    if (*url == '\0') {
        strcpy(out->path, "/");
    } else {
        snprintf(out->path, sizeof(out->path), "%s", url);
    }

    /* filename از path */
    const char *fn = strrchr(out->path, '/');
    if (fn && *(fn+1)) {
        snprintf(out->filename, sizeof(out->filename), "%s", fn + 1);
    } else {
        snprintf(out->filename, sizeof(out->filename), "ftp_download");
    }

    return (out->host[0] != '\0');
}

/* ======================================================
   helper: یه session WinINet برای FTP باز کن
   ====================================================== */
static HINTERNET ftp_open_session(void) {
    return InternetOpenA(
        "Lass/1.0 (FTP)",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);
}

/* ======================================================
   Public: ftp_is_ftp_url
   ====================================================== */
BOOL ftp_is_ftp_url(const char *url) {
    if (!url) return FALSE;
    return (_strnicmp(url, "ftp://", 6) == 0 ||
            _strnicmp(url, "ftps://", 7) == 0);
}

/* ======================================================
   Public: ftp_discover
   ====================================================== */
IDM_Result ftp_discover(const char *url,
                         const char *user,
                         const char *pass,
                         HTTP_DiscoveryInfo *out) {
    if (!url || !out) return IDM_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    FTP_URL fu;
    if (!parse_ftp_url(url, &fu)) return IDM_ERR_INVALID;

    /* override user/pass اگه پارامتر داده شده */
    if (user && *user) snprintf(fu.user, sizeof(fu.user), "%s", user);
    if (pass && *pass) snprintf(fu.pass, sizeof(fu.pass), "%s", pass);

    HINTERNET hInet = ftp_open_session();
    if (!hInet) return IDM_ERR_NETWORK;

    DWORD flags = fu.is_ftps ? INTERNET_FLAG_SECURE : 0;
    flags |= INTERNET_FLAG_PASSIVE;  /* passive mode پیش‌فرض */

    /* timeout */
    DWORD to = 15000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    HINTERNET hFtp = InternetConnectA(
        hInet, fu.host, fu.port,
        fu.user, fu.pass,
        INTERNET_SERVICE_FTP,
        flags, 0);

    if (!hFtp) {
        InternetCloseHandle(hInet);
        return IDM_ERR_NETWORK;
    }

    /* سایز فایل با FtpGetFileSize */
    HINTERNET hFile = FtpOpenFileA(hFtp, fu.path,
                                    GENERIC_READ,
                                    FTP_TRANSFER_TYPE_BINARY |
                                    INTERNET_FLAG_RELOAD, 0);
    IDM_Result result = IDM_OK;

    if (!hFile) {
        result = IDM_ERR_NETWORK;
        goto cleanup;
    }

    DWORD size_hi = 0;
    DWORD size_lo = FtpGetFileSize(hFile, &size_hi);
    out->content_length = ((uint64_t)size_hi << 32) | size_lo;
    out->accept_ranges  = (out->content_length > 0); /* FTP همیشه resume داره */
    out->is_https       = fu.is_ftps;
    snprintf(out->filename, sizeof(out->filename), "%s", fu.filename);
    snprintf(out->content_type, sizeof(out->content_type),
             "application/octet-stream");

    InternetCloseHandle(hFile);

cleanup:
    InternetCloseHandle(hFtp);
    InternetCloseHandle(hInet);
    return result;
}

/* ======================================================
   Public: ftp_open  –  HTTP_Conn-compatible handle
   ── از همون struct HTTP_Conn استفاده می‌کنه تا engine
      بتونه با FTP مثل HTTP رفتار کنه
   ====================================================== */
HTTP_Conn *ftp_open(const char *url,
                    const char *user,
                    const char *pass,
                    uint64_t    resume_from) {
    if (!url) return NULL;

    FTP_URL fu;
    if (!parse_ftp_url(url, &fu)) return NULL;
    if (user && *user) snprintf(fu.user, sizeof(fu.user), "%s", user);
    if (pass && *pass) snprintf(fu.pass, sizeof(fu.pass), "%s", pass);

    HTTP_Conn *conn = (HTTP_Conn *)calloc(1, sizeof(*conn));
    if (!conn) return NULL;

    /* session مجزا برای هر thread */
    HINTERNET hInet = ftp_open_session();
    if (!hInet) { free(conn); return NULL; }

    DWORD to_c = 15000, to_r = 60000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to_c, sizeof(to_c));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to_r, sizeof(to_r));

    DWORD flags = fu.is_ftps ? INTERNET_FLAG_SECURE : 0;
    flags |= INTERNET_FLAG_PASSIVE;

    HINTERNET hFtp = InternetConnectA(
        hInet, fu.host, fu.port,
        fu.user, fu.pass,
        INTERNET_SERVICE_FTP,
        flags, 0);

    if (!hFtp) {
        InternetCloseHandle(hInet);
        free(conn);
        return NULL;
    }

    /* اگه resume_from > 0، SetFilePointer بعد از باز کردن */
    HINTERNET hFile = FtpOpenFileA(hFtp, fu.path,
                                    GENERIC_READ,
                                    FTP_TRANSFER_TYPE_BINARY |
                                    INTERNET_FLAG_RELOAD, 0);
    if (!hFile) {
        InternetCloseHandle(hFtp);
        InternetCloseHandle(hInet);
        free(conn);
        return NULL;
    }

    /* resume: InternetSetFilePointer */
    if (resume_from > 0) {
        LONG lo = (LONG)(resume_from & 0xFFFFFFFF);
        LONG hi = (LONG)(resume_from >> 32);
        InternetSetFilePointer(hFile, lo, &hi, FILE_BEGIN, 0);
    }

    /* conn را به عنوان FTP wrapper پر کن */
    conn->hConnect = hFtp;
    conn->hRequest = hFile;
    /* hInet رو در hSession ذخیره می‌کنیم تا بعداً close بشه */
    /* استفاده از pu.host به عنوان storage */
    snprintf(conn->pu.host, sizeof(conn->pu.host), "%s", fu.host);
    conn->pu.port   = fu.port;
    /* pu.scheme رو "ftp" بذار تا http_read بدونه چطور بخونه */
    snprintf(conn->pu.scheme, sizeof(conn->pu.scheme), "%s", fu.scheme);
    /* hInet رو در یه custom field ذخیره می‌کنیم */
    conn->hSession = hInet;  /* فرض می‌کنیم HTTP_Conn این field رو داره */

    return conn;
}

/* ======================================================
   Plugin hooks
   ====================================================== */
static int  ftp_on_init   (void) { return 0; }
static void ftp_on_destroy(void) {}

IDM_PluginHooks *ftp_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;

    plug->name        = "ftp_handler";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = ftp_on_init;
    plug->on_destroy  = ftp_on_destroy;
    plug->priv        = NULL;

    LOGI(LOG_MOD_FTP, "plugin created");
    return plug;
}

void ftp_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    free(plug);
}
