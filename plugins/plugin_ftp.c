/* plugin_ftp.c — کلاینت FTP مستقل روی WinSock (رجوع کن به plugin_ftp.h) */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "plugin_ftp.h"
#include "../src/lass_log.h"

#define FTP_DEFAULT_PORT  21
#define FTP_TIMEOUT_MS    15000
#define FTP_LINE_MAX      1024
#define FTP_PATH_MAX      2048
#define FTP_ANON_USER     "anonymous"
#define FTP_ANON_PASS     "lass@example.com"

typedef struct {
    char     host[256];
    uint16_t port;
    char     user[128];
    char     pass[128];
    char     path[FTP_PATH_MAX];
    char     filename[IDM_MAX_FILENAME];
} FtpUrl;

struct FtpStream {
    SOCKET   ctrl;
    SOCKET   data;
    uint64_t pos;       /* آفست بایت بعدی که خوانده می‌شود */
    uint64_t end;       /* آخرین بایت بازه (شامل)؛ 0 = تا EOF */
    BOOL     partial;   /* سرور ادامه/بازه را پذیرفته است */
    BOOL     wsa_ok;    /* WSAStartup موفق بود و باید WSACleanup شود */
};

/* ───── ابزارهای رشته ───── */

static BOOL copy_bounded(char *dst, size_t dst_sz, const char *src, size_t len) {
    if (len + 1 > dst_sz) return FALSE;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return TRUE;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL percent_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%') {
            int hi = hexval(r[1]);
            int lo = (hi >= 0) ? hexval(r[2]) : -1;
            if (hi < 0 || lo < 0) return FALSE;
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return TRUE;
}

static BOOL has_control_chars(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return TRUE;
    return FALSE;
}

/* ───── تجزیه‌ی URL ───── */

static BOOL ftp_parse(const char *url, FtpUrl *u) {
    if (!url || has_control_chars(url)) return FALSE;
    if (_strnicmp(url, "ftp://", 6) != 0) return FALSE;
    memset(u, 0, sizeof(*u));

    const char *p = url + 6;
    const char *slash = strchr(p, '/');
    size_t alen = slash ? (size_t)(slash - p) : strlen(p);
    if (alen == 0 || alen >= 512) return FALSE;

    char auth[512];
    memcpy(auth, p, alen);
    auth[alen] = '\0';

    char *hostport = auth;
    char *at = strrchr(auth, '@');
    if (at) {
        *at = '\0';
        hostport = at + 1;
        char *colon = strchr(auth, ':');
        if (colon) {
            *colon = '\0';
            if (!copy_bounded(u->pass, sizeof(u->pass), colon + 1, strlen(colon + 1)) ||
                !percent_decode(u->pass)) return FALSE;
        }
        if (!copy_bounded(u->user, sizeof(u->user), auth, strlen(auth)) ||
            !percent_decode(u->user)) return FALSE;
        if (u->user[0] == '\0') strcpy(u->user, FTP_ANON_USER);
    } else {
        strcpy(u->user, FTP_ANON_USER);
        strcpy(u->pass, FTP_ANON_PASS);
    }

    u->port = FTP_DEFAULT_PORT;
    char *pc = strrchr(hostport, ':');
    if (pc) {
        *pc = '\0';
        long pv = strtol(pc + 1, NULL, 10);
        if (pv < 1 || pv > 65535) return FALSE;
        u->port = (uint16_t)pv;
    }
    if (hostport[0] == '\0' ||
        !copy_bounded(u->host, sizeof(u->host), hostport, strlen(hostport))) return FALSE;

    if (slash) {
        if (!copy_bounded(u->path, sizeof(u->path), slash, strlen(slash))) return FALSE;
    } else {
        strcpy(u->path, "/");
    }
    if (!percent_decode(u->path) || has_control_chars(u->path)) return FALSE;

    const char *last = strrchr(u->path, '/');
    last = last ? last + 1 : u->path;
    if (*last == '\0') return FALSE;             /* مسیر پوشه: نام فایل ندارد */
    if (!copy_bounded(u->filename, sizeof(u->filename), last, strlen(last))) return FALSE;
    return TRUE;
}

/* ───── اتصال TCP با timeout ───── */

static SOCKET tcp_connect(const char *host, uint16_t port, int timeout_ms) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char portstr[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return INVALID_SOCKET;

    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai && s == INVALID_SOCKET; ai = ai->ai_next) {
        SOCKET t = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (t == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(t, FIONBIO, &nb);
        int rc = connect(t, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wfd;
            FD_ZERO(&wfd);
            FD_SET(t, &wfd);
            struct timeval tv;
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            if (select(0, NULL, &wfd, NULL, &tv) == 1) {
                int err = 0, len = sizeof(err);
                getsockopt(t, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
                rc = (err == 0) ? 0 : SOCKET_ERROR;
            } else {
                rc = SOCKET_ERROR;
            }
        }
        if (rc == 0) {
            nb = 0;
            ioctlsocket(t, FIONBIO, &nb);
            s = t;
        } else {
            closesocket(t);
        }
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        DWORD tmo = (DWORD)timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    }
    return s;
}

/* ───── کانال کنترل ───── */

static BOOL ctrl_send_raw(SOCKET s, const char *data, size_t len) {
    while (len > 0) {
        int w = send(s, data, (int)len, 0);
        if (w <= 0) return FALSE;
        data += w;
        len  -= (size_t)w;
    }
    return TRUE;
}

/* یک پاسخ کامل FTP را می‌خواند (تک‌خطی "NNN text" یا چندخطی "NNN-..." ... "NNN text").
   کد سه‌رقمی را برمی‌گرداند و متن خط پایانی را در text می‌گذارد؛ در خطا -1. */
static int ctrl_read_reply(SOCKET s, char *text, size_t text_sz) {
    int code = -1;
    BOOL multi = FALSE;
    if (text_sz) text[0] = '\0';
    for (int guard = 0; guard < 128; guard++) {
        char line[FTP_LINE_MAX];
        size_t n = 0;
        for (;;) {
            char c;
            int r = recv(s, &c, 1, 0);
            if (r <= 0) return -1;
            if (c == '\n') break;
            if (c != '\r' && n < sizeof(line) - 1) line[n++] = c;
        }
        line[n] = '\0';
        if (n >= 3 && isdigit((unsigned char)line[0]) &&
            isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2])) {
            int this_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
            if (code < 0) {
                code  = this_code;
                multi = (n >= 4 && line[3] == '-');
                if (!multi) {
                    snprintf(text, text_sz, "%s", n > 4 ? line + 4 : "");
                    return code;
                }
                continue;
            }
            if (this_code == code && n >= 4 && line[3] == ' ') {
                snprintf(text, text_sz, "%s", line + 4);
                return code;
            }
        }
    }
    return -1;
}

/* دستور را با CRLF می‌فرستد و پاسخش را می‌خواند */
static int ctrl_cmd(SOCKET s, const char *cmd, char *text, size_t text_sz) {
    size_t n = strlen(cmd);
    char buf[FTP_PATH_MAX + 64];
    if (n + 2 >= sizeof(buf)) return -1;
    memcpy(buf, cmd, n);
    buf[n]     = '\r';
    buf[n + 1] = '\n';
    if (!ctrl_send_raw(s, buf, n + 2)) return -1;
    return ctrl_read_reply(s, text, text_sz);
}

/* ورود: پیام خوش‌آمد، USER/PASS، و TYPE I برای انتقال باینری */
static int ftp_login(SOCKET s, const FtpUrl *u) {
    char txt[256];
    char cmd[FTP_PATH_MAX];
    if (ctrl_read_reply(s, txt, sizeof(txt)) != 220) return -1;
    snprintf(cmd, sizeof(cmd), "USER %s", u->user);
    int c = ctrl_cmd(s, cmd, txt, sizeof(txt));
    if (c == 331) {
        snprintf(cmd, sizeof(cmd), "PASS %s", u->pass);
        c = ctrl_cmd(s, cmd, txt, sizeof(txt));
    }
    if (c != 230 && c != 202) return -1;
    if (ctrl_cmd(s, "TYPE I", txt, sizeof(txt)) != 200) return -1;
    return 0;
}

/* PASV: آدرس و پورت داده را از پاسخ 227 می‌خواند و اتصال داده را برقرار می‌کند */
static SOCKET ftp_pasv_connect(SOCKET ctrl, int timeout_ms) {
    char txt[256];
    if (ctrl_cmd(ctrl, "PASV", txt, sizeof(txt)) != 227) return INVALID_SOCKET;
    const char *o = strchr(txt, '(');
    int v[6];
    if (!o || sscanf(o + 1, "%d,%d,%d,%d,%d,%d",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return INVALID_SOCKET;
    for (int i = 0; i < 6; i++)
        if (v[i] < 0 || v[i] > 255) return INVALID_SOCKET;
    char host[64];
    snprintf(host, sizeof(host), "%d.%d.%d.%d", v[0], v[1], v[2], v[3]);
    return tcp_connect(host, (uint16_t)(v[4] * 256 + v[5]), timeout_ms);
}

/* ───── API عمومی ───── */

BOOL ftp_is_ftp_url(const char *url) {
    return url && (_strnicmp(url, "ftp://", 6) == 0 || _strnicmp(url, "ftps://", 7) == 0);
}

BOOL ftp_url_supported(const char *url) {
    FtpUrl u;
    return ftp_parse(url, &u);
}

IDM_Result ftp_discover(const char *url, uint64_t *size_out,
                        char *filename_out, size_t filename_sz,
                        BOOL *accept_ranges_out) {
    FtpUrl u;
    if (!ftp_parse(url, &u)) return IDM_ERR_INVALID;
    if (size_out) *size_out = 0;
    if (accept_ranges_out) *accept_ranges_out = FALSE;
    if (filename_out && filename_sz) snprintf(filename_out, filename_sz, "%s", u.filename);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return IDM_ERR_NETWORK;

    IDM_Result res = IDM_OK;
    SOCKET s = tcp_connect(u.host, u.port, FTP_TIMEOUT_MS);
    if (s == INVALID_SOCKET || ftp_login(s, &u) != 0) {
        res = IDM_ERR_NETWORK;
    } else {
        char txt[256], cmd[FTP_PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "SIZE %s", u.path);
        int c = ctrl_cmd(s, cmd, txt, sizeof(txt));
        if (c == 550) {
            res = IDM_ERR_NOT_FOUND;
        } else if (c == 213) {
            if (size_out) *size_out = (uint64_t)strtoull(txt, NULL, 10);
            if (ctrl_cmd(s, "REST 1", txt, sizeof(txt)) == 350 && accept_ranges_out)
                *accept_ranges_out = TRUE;
        }
        /* SIZE پاسخ نداد: اندازه نامشخص می‌ماند و دانلود تک‌بخشی خواهد بود */
    }
    if (s != INVALID_SOCKET) {
        ctrl_send_raw(s, "QUIT\r\n", 6);
        closesocket(s);
    }
    WSACleanup();
    return res;
}

FtpStream *ftp_range_open(const char *url, uint64_t start, uint64_t end) {
    FtpUrl u;
    if (!ftp_parse(url, &u)) return NULL;
    if (end > 0 && end < start) return NULL;

    FtpStream *st = (FtpStream *)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->ctrl = INVALID_SOCKET;
    st->data = INVALID_SOCKET;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { free(st); return NULL; }
    st->wsa_ok = TRUE;

    char txt[256], cmd[FTP_PATH_MAX + 32];
    int c = 0;
    st->ctrl = tcp_connect(u.host, u.port, FTP_TIMEOUT_MS);
    if (st->ctrl == INVALID_SOCKET || ftp_login(st->ctrl, &u) != 0) goto fail;
    if (start > 0) {
        snprintf(cmd, sizeof(cmd), "REST %llu", (unsigned long long)start);
        if (ctrl_cmd(st->ctrl, cmd, txt, sizeof(txt)) != 350) goto fail;   /* ادامه پذیرفته نشد */
    }
    st->partial = TRUE;
    st->data = ftp_pasv_connect(st->ctrl, FTP_TIMEOUT_MS);
    if (st->data == INVALID_SOCKET) goto fail;
    snprintf(cmd, sizeof(cmd), "RETR %s", u.path);
    c = ctrl_cmd(st->ctrl, cmd, txt, sizeof(txt));
    if (c != 150 && c != 125) goto fail;
    st->pos = start;
    st->end = end;
    return st;

fail:
    ftp_range_close(st);
    return NULL;
}

int ftp_range_read(FtpStream *st, void *buf, int buf_size) {
    if (!st || st->data == INVALID_SOCKET || !buf || buf_size <= 0) return -1;
    int want = buf_size;
    if (st->end > 0) {                         /* end شامل است */
        if (st->pos > st->end) return 0;
        uint64_t left = st->end - st->pos + 1;
        if ((uint64_t)want > left) want = (int)left;
    }
    int r = recv(st->data, (char *)buf, want, 0);
    if (r < 0) return -1;                      /* timeout یا reset */
    if (r == 0) {
        /* پایان اتصال داده: اگر بازه هنوز کامل نشده، قطع زودهنگام است و باید خطا باشد */
        return (st->end > 0 && st->pos <= st->end) ? -1 : 0;
    }
    st->pos += (uint64_t)r;
    return r;
}

BOOL ftp_range_got_partial(const FtpStream *st) {
    return st && st->partial;
}

void ftp_range_close(FtpStream *st) {
    if (!st) return;
    if (st->data != INVALID_SOCKET) closesocket(st->data);
    if (st->ctrl != INVALID_SOCKET) {
        ctrl_send_raw(st->ctrl, "QUIT\r\n", 6);
        closesocket(st->ctrl);
    }
    if (st->wsa_ok) WSACleanup();
    free(st);
}

IDM_PluginHooks *ftp_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    plug->name        = "ftp_handler";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->priv        = NULL;
    LOGI(LOG_MOD_FTP, "FTP client ready (independent WinSock implementation)");
    return plug;
}

void ftp_plugin_destroy(IDM_PluginHooks *plug) {
    free(plug);
}
