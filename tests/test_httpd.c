/* tests/test_httpd.c
   ─────────────────────────────────────────────────────────────
   پیاده‌سازی سرور HTTP مینیمال loopback — فقط برای test_all.c.
   جزئیات و دلیل وجودش در test_httpd.h توضیح داده شده.
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_httpd.h"

/* ── وضعیت global (این فایل فقط برای تست، یه instance واحده) ── */
static SOCKET       g_listen         = INVALID_SOCKET;
static HANDLE        g_accept_thread  = NULL;
static volatile LONG g_stop           = 0;
static volatile LONG g_active_conns   = 0; /* شامل HEAD و GET */
static volatile LONG g_active_gets    = 0;
static volatile LONG g_peak_gets      = 0;
static volatile LONG g_completed_gets = 0;
static volatile LONG g_if_range_gets = 0; /* GET رنج‌دار که If-Range معتبر داشته */
static uint32_t      g_body_size      = 0;
static uint32_t      g_trickle_ms     = 0;
static BOOL          g_wsa_started    = FALSE;

typedef struct {
    SOCKET sock;
} ConnCtx;

/* ── helper: ارسال کامل یه بافر، حتی اگه send() چند تکه لازم باشه ── */
static bool send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

/* ── parse خیلی ساده‌ی request line + هدر Range ──
   فقط چیزی که کلاینت WinINet ما (طبق http.c) واقعاً می‌فرسته رو
   پشتیبانی می‌کنه: "METHOD /path HTTP/1.1\r\n...Range: bytes=A-B\r\n" */
static void parse_request(const char *req, char *method, int method_sz,
                          bool *has_range, uint64_t *range_start,
                          uint64_t *range_end,
                          char *out_path, int out_path_sz) {
    (void)method_sz; /* بافر method همیشه با "%15s" (زیر سقف 16 بایتی که
                         caller می‌ده) پر می‌شه؛ پارامتر برای مستندسازی
                         قرارداد امضا نگه داشته شده، نه استفاده‌ی واقعی. */
    method[0] = '\0';
    if (out_path && out_path_sz > 0) out_path[0] = '\0';
    *has_range = false;
    *range_start = 0;
    *range_end = 0;

    char path[512];
    if (sscanf(req, "%15s %511s", method, path) != 2) {
        return;
    }
    if (out_path && out_path_sz > 0)
        snprintf(out_path, out_path_sz, "%s", path);

    const char *r = strstr(req, "Range:");
    if (!r) r = strstr(req, "range:");
    if (r) {
        const char *eq = strchr(r, '=');
        if (eq) {
            unsigned long long a = 0, b = 0;
            if (sscanf(eq + 1, "%llu-%llu", &a, &b) == 2) {
                *has_range = true;
                *range_start = (uint64_t)a;
                *range_end = (uint64_t)b;
            } else if (sscanf(eq + 1, "%llu-", &a) == 1) {
                *has_range = true;
                *range_start = (uint64_t)a;
                *range_end = (uint64_t)(g_body_size - 1);
            }
        }
    }
}

static DWORD WINAPI conn_thread(LPVOID param) {
    ConnCtx *ctx = (ConnCtx *)param;
    SOCKET s = ctx->sock;
    free(ctx);
    InterlockedIncrement(&g_active_conns);

    /* ── دریافت request (headers) — تا \r\n\r\n یا سقف بافر ── */
    char req[8192];
    int total = 0;
    for (;;) {
        int n = recv(s, req + total, (int)sizeof(req) - total - 1, 0);
        if (n <= 0) break;
        total += n;
        req[total] = '\0';
        if (total >= 4 && strstr(req, "\r\n\r\n")) break;
        if (total >= (int)sizeof(req) - 1) break;
    }

    if (total > 0) {
        char method[16];
        bool has_range;
        uint64_t rs, re;
        char req_path[512];
        parse_request(req, method, sizeof(method), &has_range, &rs, &re,
                     req_path, sizeof(req_path));

        bool is_head = (_stricmp(method, "HEAD") == 0);
        bool is_get  = (_stricmp(method, "GET") == 0);

        /* مسیر ویژه‌ی تست: یه HEAD روی /retry-after-test باید 429
           همراه با Retry-After: 2 برگردونه — برای تست اینکه
           http_discover واقعاً این هدر رو می‌خونه و احترام می‌ذاره
           (رجوع کن به "Retry-After honored" در test_all.c). */
        if (is_head && strstr(req_path, "retry-after-test")) {
            const char *hdr =
                "HTTP/1.1 429 Too Many Requests\r\n"
                "Retry-After: 2\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send_all(s, hdr, (int)strlen(hdr));
            shutdown(s, SD_BOTH);
            closesocket(s);
            InterlockedDecrement(&g_active_conns);
            return 0;
        }

        if (is_head) {
            char hdr[256];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: %u\r\n"
                "Accept-Ranges: bytes\r\n"
                "ETag: \"lass-test-etag-1\"\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Connection: close\r\n\r\n",
                (unsigned)g_body_size);
            send_all(s, hdr, hlen);
        } else if (is_get) {
            if (has_range && strstr(req, "If-Range: \"lass-test-etag-1\""))
                InterlockedIncrement(&g_if_range_gets);
            InterlockedIncrement(&g_active_gets);
            /* peak رو با یه compare-loop ساده به‌روز کن (فقط چند
               thread همزمان، رقابت واقعی نداره برای این مقیاس) */
            for (;;) {
                LONG cur = g_active_gets;
                LONG peak = g_peak_gets;
                if (cur <= peak) break;
                if (InterlockedCompareExchange(&g_peak_gets, cur, peak) == peak) break;
            }

            uint64_t start = has_range ? rs : 0;
            uint64_t end   = has_range ? re : (uint64_t)(g_body_size - 1);
            if (end >= g_body_size) end = g_body_size - 1;
            if (start > end) start = end;
            uint64_t len = end - start + 1;

            char hdr[256];
            int hlen;
            if (has_range) {
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 206 Partial Content\r\n"
                    "Content-Length: %llu\r\n"
                    "Content-Range: bytes %llu-%llu/%u\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Connection: close\r\n\r\n",
                    (unsigned long long)len,
                    (unsigned long long)start, (unsigned long long)end,
                    (unsigned)g_body_size);
            } else {
                hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: %llu\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Connection: close\r\n\r\n",
                    (unsigned long long)len);
            }

            if (send_all(s, hdr, hlen)) {
                /* ── بدنه رو در چند تکه با فاصله بفرست تا connection
                   واقعاً چند صدم ثانیه باز بمونه — این چیزیه که
                   امکان اندازه‌گیری هم‌زمانی واقعی رو می‌ده. */
                const int NUM_CHUNKS = 8;
                char chunk[4096];
                memset(chunk, 'A', sizeof(chunk));
                uint64_t sent_total = 0;
                DWORD sleep_per_chunk = g_trickle_ms / NUM_CHUNKS;

                for (int i = 0; i < NUM_CHUNKS && sent_total < len; i++) {
                    uint64_t remaining = len - sent_total;
                    uint64_t this_chunk = remaining / (NUM_CHUNKS - i);
                    if (this_chunk == 0) this_chunk = remaining;
                    while (this_chunk > 0) {
                        int piece = (int)((this_chunk > sizeof(chunk)) ? sizeof(chunk) : this_chunk);
                        if (!send_all(s, chunk, piece)) { this_chunk = 0; sent_total = len; break; }
                        sent_total += (uint64_t)piece;
                        this_chunk -= (uint64_t)piece;
                    }
                    if (i < NUM_CHUNKS - 1 && sleep_per_chunk > 0) Sleep(sleep_per_chunk);
                }
            }

            InterlockedDecrement(&g_active_gets);
            InterlockedIncrement(&g_completed_gets);
        }
        /* هر متد دیگه‌ای: بدون پاسخ، connection بسته می‌شه — کافیه
           چون کلاینت ما فقط HEAD/GET می‌فرسته. */
    }

    shutdown(s, SD_BOTH);
    closesocket(s);
    InterlockedDecrement(&g_active_conns);
    return 0;
}

static DWORD WINAPI accept_thread(LPVOID param) {
    (void)param;
    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_listen, &rfds);
        struct timeval tv = {0, 200000}; /* 200ms — برای چک دوره‌ای g_stop */

        int r = select(0, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;
        if (g_stop) break;

        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) continue;

        ConnCtx *ctx = (ConnCtx *)malloc(sizeof(ConnCtx));
        if (!ctx) { closesocket(c); continue; }
        ctx->sock = c;

        HANDLE h = CreateThread(NULL, 0, conn_thread, ctx, 0, NULL);
        if (h) CloseHandle(h); /* thread خودش self-cleaning هست */
        else { free(ctx); closesocket(c); }
    }
    return 0;
}

bool test_httpd_start(uint16_t port, uint32_t body_size, uint32_t trickle_total_ms) {
    if (g_listen != INVALID_SOCKET) return false; /* از قبل running */

    if (!g_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        g_wsa_started = TRUE;
    }

    g_body_size      = body_size;
    g_trickle_ms     = trickle_total_ms;
    g_stop           = 0;
    g_active_conns   = 0;
    g_active_gets    = 0;
    g_peak_gets      = 0;
    g_completed_gets = 0;
    g_if_range_gets = 0;

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    if (listen(g_listen, 32) == SOCKET_ERROR) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }

    g_accept_thread = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    if (!g_accept_thread) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    return true;
}

void test_httpd_stop(void) {
    if (g_listen == INVALID_SOCKET) return;

    g_stop = 1;

    /* بستن listen socket، select/accept رو بیدار می‌کنه */
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;

    if (g_accept_thread) {
        WaitForSingleObject(g_accept_thread, 2000);
        CloseHandle(g_accept_thread);
        g_accept_thread = NULL;
    }

    /* صبر کوتاه برای اینکه connection thread های در حال کار
       (اگه هنوز مونده باشن) به پایان برسن، قبل از WSACleanup — این
       جلوی بستن socketهای هنوز فعال از زیر پای thread دیگه رو
       می‌گیره. */
    for (int i = 0; i < 20 && g_active_conns > 0; i++) Sleep(100);

    if (g_wsa_started) {
        WSACleanup();
        g_wsa_started = FALSE;
    }
}

int test_httpd_peak_concurrent_gets(void) { return (int)g_peak_gets; }
int test_httpd_completed_gets(void)       { return (int)g_completed_gets; }
int test_httpd_if_range_gets(void)        { return (int)g_if_range_gets; }
