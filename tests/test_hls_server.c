/* tests/test_hls_server.c — سرور تست HLS (رجوع کن به test_hls_server.h) */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_hls_server.h"

#define HLS_TS_ROUTES 48
#define HLS_TS_PATH   160

typedef struct {
    BOOL           used;
    char           path[HLS_TS_PATH];
    const uint8_t *body;
    uint32_t       len;
    int            fail_status;   /* 0 یعنی بدون خطا */
    int            fail_left;     /* -1 یعنی همیشه؛ 0 یعنی تمام؛ وگرنه باقی‌مانده */
    int            hits;
} HlsTsRoute;

static HlsTsRoute       g_routes[HLS_TS_ROUTES];
static CRITICAL_SECTION g_lock;
static BOOL             g_lock_ready = FALSE;
static BOOL             g_wsa_ready  = FALSE;
static SOCKET           g_listen     = INVALID_SOCKET;
static HANDLE           g_thread     = NULL;
static volatile LONG    g_running    = 0;

/* باید زیر g_lock صدا زده شود */
static HlsTsRoute *route_find(const char *path, BOOL create) {
    for (int i = 0; i < HLS_TS_ROUTES; i++)
        if (g_routes[i].used && strcmp(g_routes[i].path, path) == 0)
            return &g_routes[i];
    if (!create) return NULL;
    for (int i = 0; i < HLS_TS_ROUTES; i++) {
        if (!g_routes[i].used) {
            memset(&g_routes[i], 0, sizeof(g_routes[i]));
            g_routes[i].used = TRUE;
            snprintf(g_routes[i].path, sizeof(g_routes[i].path), "%s", path);
            return &g_routes[i];
        }
    }
    return NULL;
}

void hls_test_server_set(const char *path, const uint8_t *body, uint32_t len) {
    EnterCriticalSection(&g_lock);
    HlsTsRoute *r = route_find(path, TRUE);
    if (r) {
        r->body = body;
        r->len = len;
    }
    LeaveCriticalSection(&g_lock);
}

void hls_test_server_fail(const char *path, int status, int times) {
    EnterCriticalSection(&g_lock);
    HlsTsRoute *r = route_find(path, TRUE);
    if (r) {
        r->fail_status = status;
        r->fail_left = times > 0 ? times : -1;
    }
    LeaveCriticalSection(&g_lock);
}

void hls_test_server_clear_fail(const char *path) {
    EnterCriticalSection(&g_lock);
    HlsTsRoute *r = route_find(path, FALSE);
    if (r) {
        r->fail_status = 0;
        r->fail_left = 0;
    }
    LeaveCriticalSection(&g_lock);
}

int hls_test_server_hits(const char *path) {
    int n = 0;
    EnterCriticalSection(&g_lock);
    HlsTsRoute *r = route_find(path, FALSE);
    if (r) n = r->hits;
    LeaveCriticalSection(&g_lock);
    return n;
}

void hls_test_server_reset_hits(void) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < HLS_TS_ROUTES; i++) g_routes[i].hits = 0;
    LeaveCriticalSection(&g_lock);
}

static const char *reason_of(int status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 410: return "Gone";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Status";
    }
}

static void send_all(SOCKET s, const void *data, size_t n) {
    const char *p = (const char *)data;
    while (n > 0) {
        int chunk = n > 65536u ? 65536 : (int)n;
        int k = send(s, p, chunk, 0);
        if (k <= 0) return;
        p += k;
        n -= (size_t)k;
    }
}

static void handle_conn(SOCKET cs) {
    DWORD tmo = 5000;
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));

    char req[4096];
    int got = 0;
    while (got < (int)sizeof(req) - 1) {
        int k = recv(cs, req + got, (int)sizeof(req) - 1 - got, 0);
        if (k <= 0) break;
        got += k;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[got] = '\0';
    if (strncmp(req, "GET ", 4) != 0) {
        closesocket(cs);
        return;
    }

    char path[HLS_TS_PATH] = "";
    size_t pn = 0;
    const char *p = req + 4;
    while (p[pn] && p[pn] != ' ' && pn < sizeof(path) - 1) {
        path[pn] = p[pn];
        pn++;
    }
    path[pn] = '\0';

    BOOL has_range = FALSE, has_end = FALSE;
    unsigned long long rs = 0, re = 0;
    const char *rh = strstr(req, "Range: bytes=");
    if (rh) {
        char *ep = NULL;
        has_range = TRUE;
        rh += 13;
        rs = strtoull(rh, &ep, 10);
        if (ep && *ep == '-' && ep[1] >= '0' && ep[1] <= '9') {
            re = strtoull(ep + 1, NULL, 10);
            has_end = TRUE;
        }
    }

    static const uint8_t err_body[] = "error\n";
    int status = 404;
    const uint8_t *body = err_body;
    uint32_t blen = (uint32_t)(sizeof(err_body) - 1);
    unsigned long long cr_start = 0, cr_end = 0, total = 0;
    BOOL partial = FALSE;

    EnterCriticalSection(&g_lock);
    HlsTsRoute *r = route_find(path, FALSE);
    if (r) {
        r->hits++;
        if (r->fail_status != 0 && r->fail_left != 0) {
            status = r->fail_status;
            if (r->fail_left > 0) r->fail_left--;
        } else if (r->body) {
            status = 200;
            body = r->body;
            blen = r->len;
            total = r->len;
            if (has_range) {
                if (total == 0 || rs >= total || (has_end && re < rs)) {
                    status = 416;
                    body = err_body;
                    blen = (uint32_t)(sizeof(err_body) - 1);
                } else {
                    unsigned long long e0 = has_end ? re : total - 1u;
                    if (e0 >= total) e0 = total - 1u;
                    status = 206;
                    body = r->body + rs;
                    blen = (uint32_t)(e0 - rs + 1u);
                    cr_start = rs;
                    cr_end = e0;
                    partial = TRUE;
                }
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    char head[512];
    int hl;
    if (partial)
        hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %llu-%llu/%llu\r\n",
                      cr_start, cr_end, total);
    else
        hl = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", status, reason_of(status));
    hl += snprintf(head + hl, sizeof(head) - (size_t)hl,
                   "Content-Length: %u\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n",
                   (unsigned int)blen);
    send_all(cs, head, (size_t)hl);
    send_all(cs, body, blen);
    closesocket(cs);
}

static DWORD WINAPI hls_server_thread(LPVOID unused) {
    (void)unused;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        SOCKET cs = accept(g_listen, NULL, NULL);
        if (cs == INVALID_SOCKET) break;
        handle_conn(cs);
    }
    return 0;
}

BOOL hls_test_server_start(uint16_t port) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = TRUE;
    }
    if (!g_wsa_ready) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return FALSE;
        g_wsa_ready = TRUE;
    }
    EnterCriticalSection(&g_lock);
    memset(g_routes, 0, sizeof(g_routes));
    LeaveCriticalSection(&g_lock);

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return FALSE;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(g_listen, 8) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return FALSE;
    }
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, hls_server_thread, NULL, 0, NULL);
    if (!g_thread) {
        InterlockedExchange(&g_running, 0);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

void hls_test_server_stop(void) {
    InterlockedExchange(&g_running, 0);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}
