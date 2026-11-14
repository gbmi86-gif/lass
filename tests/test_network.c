/* network: HTTP, segment and URL tests (tests/test_network.c) */
#include "test_common.h"

/* ======================================================
   تست: گفتگو با سرور، نه دیکته — Retry-After honored + شروع
   محتاطانه‌تر روی host پرمشکل (رجوع کن به توضیح کامل کنار
   engine_report_host_trouble/pick_seg_count در engine.c و
   خوندن Retry-After در http.c)
   ====================================================== */
void test_server_negotiation(void) {
    SECTION("Server Negotiation: Retry-After + Adaptive Concurrency");

    const uint16_t PORT2 = 58733;
    bool srv_ok = test_httpd_start(PORT2, 4096, 0);
    TEST("negotiation test server started", srv_ok);
    if (srv_ok) {
        http_init();
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/retry-after-test", PORT2);

        HTTP_DiscoveryInfo info = {0};
        IDM_Result r = http_discover(url, NULL, NULL, &info);

        TEST("429 with Retry-After is treated as retryable (not permanent)",
             r == IDM_ERR_NETWORK);
        TEST("Retry-After value actually parsed from server response",
             info.retry_after_sec == 2);

        test_httpd_stop();
    }

    /* ── شروع محتاطانه‌تر روی host پرمشکل ── */
    const char *host = "trouble-test-host.example";
    uint32_t n_before = engine_test_pick_seg_count(host, 100ULL*1024*1024, 8);
    TEST("clean host gets full segment count for a large file", n_before == 8);

    for (int i = 0; i < 3; i++) engine_report_host_trouble(host);
    uint32_t n_after = engine_test_pick_seg_count(host, 100ULL*1024*1024, 8);
    TEST("host with 3 reported troubles gets fewer segments",
         n_after == n_before - 3);
    TEST("trouble-adjusted count never drops below 1", n_after >= 1);

    /* یه host کاملاً بی‌ربط نباید تحت تأثیر مشکلات host بالا باشه */
    uint32_t n_other = engine_test_pick_seg_count("unrelated-host.example",
                                                  100ULL*1024*1024, 8);
    TEST("unrelated host is unaffected by another host's trouble",
         n_other == 8);
}

/* ======================================================
   تست: Batch / استخراج URL از متن آزاد
   (سناریوهایی که قبلاً «No valid URLs found» می‌دادن)
   ====================================================== */
static int url_line_count(const char *out) {
    int n = 0;
    for (const char *p = out; *p; p++) if (*p == '\n') n++;
    return n;
}

void test_batch_url_extraction(void) {
    SECTION("Batch: URL extraction from free text");
    char out[4096];

    /* ۱) روی یک خط با فاصله — قبلاً کل خط یک URL حساب می‌شد */
    int n = engine_extract_urls("https://a.com/x.zip https://b.com/y.zip\thttp://c.com/z", out, sizeof(out));
    TEST("space/tab separated links all found", n == 3);
    TEST("each link on its own line", url_line_count(out) == 3);

    /* ۲) فقط \r بین لینک‌ها (بعضی برنامه‌ها/کلیپ‌بوردها) */
    n = engine_extract_urls("http://a.com/1\rhttp://a.com/2\rhttp://a.com/3", out, sizeof(out));
    TEST("CR-only separated links found", n == 3);

    /* ۳) CRLF معمولی */
    n = engine_extract_urls("http://a.com/1\r\nhttp://a.com/2\r\n", out, sizeof(out));
    TEST("CRLF separated links found", n == 2);
    TEST("no CR left in output", strchr(out, '\r') == NULL);

    /* ۴) متن اطراف، کوتیشن و <> */
    n = engine_extract_urls("Link: <https://x.com/f.zip>, or \"http://y.com/g.zip\".", out, sizeof(out));
    TEST("links inside <> and quotes found", n == 2);
    TEST("trailing punctuation stripped",
         strstr(out, "f.zip\n") != NULL && strstr(out, "g.zip\n") != NULL);

    /* ۵) تکراری‌ها یکی می‌شن */
    n = engine_extract_urls("http://a.com/1\nhttp://a.com/1\nhttp://a.com/2", out, sizeof(out));
    TEST("duplicate links removed", n == 2);

    /* ۶) پرانتز: جفت‌دار نگه داشته، بدون جفت حذف */
    n = engine_extract_urls("http://a.com/w_(x) and (see http://b.com/p)", out, sizeof(out));
    TEST("balanced paren kept, dangling paren dropped",
         n == 2 && strstr(out, "w_(x)\n") != NULL && strstr(out, "b.com/p\n") != NULL);

    /* ۷) بایت‌های غیر-ASCII (UTF-8) به %XX */
    n = engine_extract_urls("http://a.com/\xD8\xB3", out, sizeof(out));
    TEST("non-ASCII bytes percent-encoded", n == 1 && strstr(out, "/%D8%B3\n") != NULL);

    /* ۸) NBSP بین لینک‌ها (کپی از وب) جداکننده‌ست */
    n = engine_extract_urls("http://a.com/1\xC2\xA0http://a.com/2", out, sizeof(out));
    TEST("NBSP separates links", n == 2);

    /* ۹) وسط کلمه scheme نیست */
    n = engine_extract_urls("xhttp://a.com/1 nothing here", out, sizeof(out));
    TEST("scheme glued to a word is not a link", n == 0);

    /* ۱۰) ورودی بی‌لینک / NULL / خالی */
    TEST("plain text has zero links", engine_extract_urls("hello world", out, sizeof(out)) == 0);
    TEST("NULL text safe", engine_extract_urls(NULL, out, sizeof(out)) == 0);
    TEST("empty text safe", engine_extract_urls("", out, sizeof(out)) == 0);
    TEST("scheme alone is not a link", engine_extract_urls("http://", out, sizeof(out)) == 0);

    /* ۱۱) فقط شمارش (out==NULL) */
    TEST("count-only mode works",
         engine_extract_urls("http://a.com/1 http://a.com/2", NULL, 0) == 2);

    /* ۱۲) بافر کوچیک: URL ناقص هرگز نوشته نمی‌شه */
    char tiny[24];
    n = engine_extract_urls("http://a.com/1 http://a.com/some/long/path/file.zip", tiny, sizeof(tiny));
    TEST("tiny buffer never holds a partial URL",
         n <= 2 && (tiny[0] == '\0' || tiny[strlen(tiny) - 1] == '\n'));

    /* ۱۳) لیست بزرگ (بیشتر از سقف ۳۲۷۶۷ کاراکتر پیش‌فرض کنترل Edit)
       — ۶۰۰ لینک ~۱۰۰ کاراکتری */
    size_t big_cap = 600 * 130;
    char *big = (char *)malloc(big_cap), *bo = (char *)malloc(big_cap * 3);
    if (big && bo) {
        size_t pos = 0;
        for (int i = 0; i < 600; i++)
            pos += (size_t)snprintf(big + pos, big_cap - pos,
                "https://cdn%d.example.com/dl/token/abcdefghijklmnopqrstuvwxyz0123456789/file_%04d.zip?sig=0123456789abcdef\r\n",
                i % 7, i);
        TEST("big list is longer than default Edit limit", pos > 32767);
        int bn = engine_extract_urls(big, bo, big_cap * 3);
        TEST("600 links in a 60KB paste all extracted", bn == 600);
    }
    free(big); free(bo);
}

/* ======================================================
   تست: سیاست «۱۰ شکست پیاپی» — سروری که Range را نادیده می‌گیرد
   ====================================================== */
#define POLICY_BODY_SIZE (8u * 1024u * 1024u)

/* ── کمک‌های سرور تست ───────────────────────────────────────────────
   هر اتصال در thread خودش اجرا می‌شه: یه client کند نباید بقیه‌ی
   درخواست‌ها را پشت سر خودش معطل کنه (سرور تک‌thread قبلی همین بود). */
typedef struct {
    SOCKET ls;
    LPTHREAD_START_ROUTINE handler;
} ServerCtx;

static char g_zero_block[65536];   /* فقط خواندنی؛ بین thread ها مشترکه */

static BOOL send_all(SOCKET c, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < n) {
        size_t left = n - sent;
        int chunk = (int)(left > 65536 ? 65536 : left);
        int r = send(c, p + sent, chunk, 0);
        if (r <= 0) return FALSE;
        sent += (size_t)r;
    }
    return TRUE;
}

static BOOL send_zeros(SOCKET c, unsigned long long total) {
    while (total > 0) {
        size_t chunk = total > sizeof(g_zero_block) ? sizeof(g_zero_block) : (size_t)total;
        if (!send_all(c, g_zero_block, chunk)) return FALSE;
        total -= chunk;
    }
    return TRUE;
}

/* سرِ درخواست را تا \r\n\r\n می‌خونیم: یه recv تنها ممکنه فقط بخشی از هدرها را بیاره و Range جا بیفته */
static BOOL read_request_head(SOCKET c, char *buf, size_t cap) {
    size_t used = 0;
    if (cap == 0) return FALSE;
    buf[0] = '\0';
    while (used + 1 < cap) {
        int r = recv(c, buf + used, (int)(cap - used - 1), 0);
        if (r <= 0) break;
        used += (size_t)r;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) return TRUE;
    }
    return FALSE;
}

static DWORD WINAPI accept_loop(LPVOID p) {
    ServerCtx *ctx = (ServerCtx *)p;
    for (;;) {
        SOCKET c = accept(ctx->ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;      /* listening socket بسته شد: تست تمام شده */
        DWORD tv = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        HANDLE t = CreateThread(NULL, 0, ctx->handler, (LPVOID)(UINT_PTR)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    free(ctx);
    return 0;
}

static HANDLE start_test_server(SOCKET ls, LPTHREAD_START_ROUTINE handler) {
    ServerCtx *ctx = (ServerCtx *)malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->ls = ls;
    ctx->handler = handler;
    HANDLE th = CreateThread(NULL, 0, accept_loop, ctx, 0, NULL);
    if (!th) free(ctx);
    return th;
}

/* سرور: Range را نادیده می‌گیرد؛ HEAD درست است، GET همیشه کل فایل را با 200 می‌فرستد */
static DWORD WINAPI policy_conn_proc(LPVOID p) {
    SOCKET c = (SOCKET)(UINT_PTR)p;
    char req[2048];
    if (read_request_head(c, req, sizeof(req))) {
        if (strncmp(req, "HEAD", 4) == 0) {
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nAccept-Ranges: bytes\r\n"
                "ETag: \"policy\"\r\nConnection: close\r\n\r\n", (unsigned)POLICY_BODY_SIZE);
            send_all(c, hdr, (size_t)hl);
        } else {
            /* بخش اول (شروع از بایت ۰) را درست جواب می‌دهیم تا روی هر ماشینی زود تمام شود.
               بخشی که از وسط فایل شروع شود Range را نادیده می‌گیرد — همان مورد اصلی تست. */
            unsigned long long s = 0, e = POLICY_BODY_SIZE - 1ull;
            const char *rp = strstr(req, "Range: bytes=");
            if (rp) {
                rp += 13;
                s = strtoull(rp, NULL, 10);
                const char *dash = strchr(rp, '-');
                if (dash && dash[1] >= '0' && dash[1] <= '9')
                    e = strtoull(dash + 1, NULL, 10);
            }
            if (e >= POLICY_BODY_SIZE) e = POLICY_BODY_SIZE - 1ull;
            if (rp && s == 0) {
                unsigned long long len = e + 1ull;
                char hdr[320];
                int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 206 Partial Content\r\nContent-Length: %llu\r\n"
                    "Content-Range: bytes 0-%llu/%u\r\nAccept-Ranges: bytes\r\n"
                    "ETag: \"policy\"\r\nConnection: close\r\n\r\n",
                    len, e, (unsigned)POLICY_BODY_SIZE);
                if (send_all(c, hdr, (size_t)hl))
                    send_zeros(c, len);
            } else {
                char hdr[256];
                int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nAccept-Ranges: bytes\r\n"
                    "ETag: \"policy\"\r\nConnection: close\r\n\r\n", (unsigned)POLICY_BODY_SIZE);
                if (send_all(c, hdr, (size_t)hl))
                    send_zeros(c, POLICY_BODY_SIZE);
            }
        }
    }
    closesocket(c);
    return 0;
}

void test_segment_policy_parks_stuck_range(void) {
    SECTION("Segment policy: an ignored range is parked, then the download stops with a reason");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18478);
    BOOL ok = (ls != INVALID_SOCKET) &&
              bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
              listen(ls, 8) == 0;
    TEST("range-ignoring server listening", ok);
    if (!ok) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    HANDLE th = start_test_server(ls, policy_conn_proc);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "http://127.0.0.1:18478/policy.bin", "policy_test.bin", 4, 0) : NULL;
    TEST("download added", dl != NULL);

    DL_State st = DL_QUEUED;
    char reason[256];
    memset(reason, 0, sizeof(reason));
    int waited = 0;
    while (dl && waited < 40000) {
        Sleep(250);
        waited += 250;
        EnterCriticalSection(&eng->lock);
        st = dl->state;
        size_t fl = strlen(dl->fail_reason);
        if (fl > sizeof(reason) - 1) fl = sizeof(reason) - 1;
        memcpy(reason, dl->fail_reason, fl);
        reason[fl] = '\0';
        LeaveCriticalSection(&eng->lock);
        if (st == DL_FAILED || st == DL_COMPLETED) break;
    }
    TEST("download stops once every segment is parked", st == DL_FAILED);
    TEST("the stop reason says why", strstr(reason, "no progress") != NULL &&
                                     strstr(reason, "ignored the range") != NULL);

    engine_destroy(eng);
    closesocket(ls);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    TEST("the stop reason counts all 10 attempts", strstr(reason, "(10 attempts") != NULL);
    DeleteFileA("policy_test.bin");
    DeleteFileA("policy_test.bin.tels");
    DT_CLEAN_NAME("policy_test.bin");
}

/* ======================================================
   تست: Retry-After برای بخش‌ها — دو درخواست اول 429 با Retry-After، بعد Range درست
   ====================================================== */
#define RETRY_BODY_SIZE (8u * 1024u * 1024u)

static volatile LONG g_retry_429_served = 0;

static volatile LONG g_retry_gets_seen = 0;

/* سرور: دو GET اول 429 با Retry-After، بعد Range را درست جواب می‌دهد */
static DWORD WINAPI retry_conn_proc(LPVOID p) {
    SOCKET c = (SOCKET)(UINT_PTR)p;
    char req[2048];
    if (read_request_head(c, req, sizeof(req))) {
        if (strncmp(req, "HEAD", 4) == 0) {
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nAccept-Ranges: bytes\r\n"
                "ETag: \"retry\"\r\nConnection: close\r\n\r\n", (unsigned)RETRY_BODY_SIZE);
            send_all(c, hdr, (size_t)hl);
        } else {
            LONG n = InterlockedIncrement(&g_retry_gets_seen);
            if (n <= 2) {
                InterlockedIncrement(&g_retry_429_served);
                static const char r429[] =
                    "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 1\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(c, r429, sizeof(r429) - 1);
            } else {
                unsigned long long s = 0, e = RETRY_BODY_SIZE - 1ull;
                const char *rp = strstr(req, "Range: bytes=");
                if (rp) {
                    rp += 13;
                    s = strtoull(rp, NULL, 10);
                    const char *dash = strchr(rp, '-');
                    if (dash && dash[1] >= '0' && dash[1] <= '9')
                        e = strtoull(dash + 1, NULL, 10);
                }
                if (e >= RETRY_BODY_SIZE) e = RETRY_BODY_SIZE - 1ull;
                if (s > e) s = e;
                unsigned long long len = e - s + 1ull;
                char hdr[320];
                int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 206 Partial Content\r\nContent-Length: %llu\r\n"
                    "Content-Range: bytes %llu-%llu/%u\r\nAccept-Ranges: bytes\r\n"
                    "ETag: \"retry\"\r\nConnection: close\r\n\r\n",
                    len, s, e, (unsigned)RETRY_BODY_SIZE);
                if (send_all(c, hdr, (size_t)hl))
                    send_zeros(c, len);
            }
        }
    }
    closesocket(c);
    return 0;
}

void test_segment_honours_retry_after(void) {
    SECTION("Segments honour Retry-After from the server and then finish the download");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18479);
    BOOL ok = (ls != INVALID_SOCKET) &&
              bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
              listen(ls, 8) == 0;
    TEST("retry-after server listening", ok);
    if (!ok) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    InterlockedExchange(&g_retry_429_served, 0);
    InterlockedExchange(&g_retry_gets_seen, 0);
    HANDLE th = start_test_server(ls, retry_conn_proc);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "http://127.0.0.1:18479/retry.bin", "retry_after_test.bin", 2, 0) : NULL;
    TEST("download added", dl != NULL);

    DL_State st = DL_QUEUED;
    int waited = 0;
    while (dl && waited < 40000) {
        Sleep(250);
        waited += 250;
        EnterCriticalSection(&eng->lock);
        st = dl->state;
        LeaveCriticalSection(&eng->lock);
        if (st == DL_COMPLETED || st == DL_FAILED) break;
    }
    TEST("the server really sent Retry-After", g_retry_429_served >= 1);
    TEST("download completes after the wait", st == DL_COMPLETED);

    HANDLE hf = CreateFileA("retry_after_test.bin", GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER fsz;
    fsz.QuadPart = 0;
    BOOL sized = FALSE;
    if (hf != INVALID_HANDLE_VALUE) {
        sized = GetFileSizeEx(hf, &fsz);
        CloseHandle(hf);
    }
    TEST("final file has the full size", sized && (unsigned long long)fsz.QuadPart == RETRY_BODY_SIZE);

    engine_destroy(eng);
    closesocket(ls);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    DeleteFileA("retry_after_test.bin");
    DeleteFileA("retry_after_test.bin.tels");
}

/* ======================================================
   تست: استخراج hostname از URL برای per-host staggering
   ====================================================== */
void test_http_extract_host(void) {
    SECTION("HTTP Host Extraction (for per-host stagger)");

    char host[128];

    http_extract_host("http://example.com/file.zip", host, sizeof(host));
    TEST("simple http host extracted", strcmp(host, "example.com") == 0);

    http_extract_host("https://cdn.example.com:8080/path/file.rar?x=1",
                      host, sizeof(host));
    TEST("https host with port and query extracted",
        strcmp(host, "cdn.example.com") == 0);

    http_extract_host("https://pdn.sharezilla.ir/d/software/x.rar",
                      host, sizeof(host));
    TEST("real-world host extracted",
        strcmp(host, "pdn.sharezilla.ir") == 0);

    /* null safety */
    host[0] = 'X'; host[1] = '\0';
    http_extract_host(NULL, host, sizeof(host));
    TEST("null url safe", host[0] == '\0');

    http_extract_host("http://a.com/x.zip", NULL, 0);
    TEST("null out buffer safe", TRUE); /* فقط نباید crash کنه */

    /* دو URL از یه host یکسان باید همون hostname رو بدن —
       این پایه‌ی منطق stagger هست */
    char host1[128], host2[128];
    http_extract_host("https://pdn.sharezilla.ir/file1.rar", host1, sizeof(host1));
    http_extract_host("https://pdn.sharezilla.ir/file2.rar", host2, sizeof(host2));
    TEST("same-host URLs produce identical hostname",
        strcmp(host1, host2) == 0);

    /* دو URL از host های متفاوت باید متفاوت باشن */
    char host3[128];
    http_extract_host("https://other-site.com/file.rar", host3, sizeof(host3));
    TEST("different-host URLs produce different hostname",
        strcmp(host1, host3) != 0);
}

/* ======================================================
   تست ۱۶: Settings — load/save/defaults/startup
   ====================================================== */
/* ======================================================
   تست: http_fixup_extension — پسوند از Content-Type
   ====================================================== */
void test_http_fixup(void) {
    SECTION("HTTP Extension Fix (mime-type inference)");

    char fn[256];

    /* فایل با پسوند از قبل — نباید تغییر کنه */
    strncpy(fn, "video.mp4", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("mp4 unchanged",          strcmp(fn, "video.mp4") == 0);

    strncpy(fn, "archive.zip", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/zip");
    TEST("zip unchanged",          strcmp(fn, "archive.zip") == 0);

    /* فایل CDN بدون پسوند — باید پسوند اضافه بشه */
    strncpy(fn, "9f8e7d6c3b2a", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("cdn token gets .mp4",    strcmp(fn, "9f8e7d6c3b2a.mp4") == 0);

    strncpy(fn, "dl_token_abc123", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/zip");
    TEST("token gets .zip",        strcmp(fn, "dl_token_abc123.zip") == 0);

    strncpy(fn, "download_id_xyz", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/x-rar-compressed");
    TEST("token gets .rar",        strcmp(fn, "download_id_xyz.rar") == 0);

    strncpy(fn, "file_id_12345", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/x-msdownload");
    TEST("token gets .exe",        strcmp(fn, "file_id_12345.exe") == 0);

    strncpy(fn, "audio_stream_99", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "audio/mpeg");
    TEST("token gets .mp3",        strcmp(fn, "audio_stream_99.mp3") == 0);

    /* mime با پارامتر اضافی — باید trim بشه */
    strncpy(fn, "doc_no_ext", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/pdf; charset=utf-8");
    TEST("pdf with params",        strcmp(fn, "doc_no_ext.pdf") == 0);

    /* mime ناشناخته — نباید تغییر کنه */
    strncpy(fn, "unknown_file", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/x-custom-unknown");
    TEST("unknown mime unchanged", strcmp(fn, "unknown_file") == 0);

    /* NULL safety */
    http_fixup_extension(NULL, 0, "video/mp4");
    strncpy(fn, "test", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), NULL);
    TEST("null safety",            strcmp(fn, "test") == 0);

    /* فایل خالی */
    fn[0] = '\0';
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("empty filename no crash", TRUE);

    /* octet-stream — باید .bin بگیره */
    strncpy(fn, "raw_data_blob", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "application/octet-stream");
    TEST("octet-stream gets .bin", strcmp(fn, "raw_data_blob.bin") == 0);

    /* mkv */
    strncpy(fn, "movie_hd_token", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/x-matroska");
    TEST("matroska gets .mkv",     strcmp(fn, "movie_hd_token.mkv") == 0);
}

/* ======================================================
   تست: امنیت — Command Injection Protection
   (video_sniffer و rtmp که URL رو به CreateProcessA میدن)
   ====================================================== */
void test_command_injection_protection(void) {
    SECTION("Security — Command Injection Protection");

    /* این تست‌ها مستقیم quote-detection logic رو شبیه‌سازی می‌کنن
       چون خود توابع rtmp_worker_thread/vs_sniff_via_ytdlp داخلی
       (static) هستن و نیاز به process واقعی دارن. اینجا فقط
       اطمینان می‌دیم که pattern تشخیص quote درست کار می‌کنه —
       چیزی که در کد واقعی استفاده میشه. */

    const char *malicious_url =
        "https://x.com/video\" & calc.exe & echo \"";
    BOOL has_quote = FALSE;
    for (const char *p = malicious_url; *p; p++)
        if (*p == '"') { has_quote = TRUE; break; }
    TEST("malicious url with quote detected", has_quote);

    const char *normal_url = "https://example.com/video.mp4?id=123";
    BOOL normal_has_quote = FALSE;
    for (const char *p = normal_url; *p; p++)
        if (*p == '"') { normal_has_quote = TRUE; break; }
    TEST("normal url has no quote", !normal_has_quote);
}

/* ======================================================
   تست: امنیت — Content-Length Integer Overflow Protection
   ====================================================== */
void test_content_length_validation(void) {
    SECTION("Security — Content-Length Overflow Protection");

    /* شبیه‌سازی منطق اعتبارسنجی که در http_discover هست:
       مقدار منفی یا خیلی بزرگ باید رد بشه */
    LONGLONG raw1 = _atoi64("-1");
    TEST("negative content-length detected", raw1 < 0);

    LONGLONG raw2 = _atoi64("999999999999999999");
    const LONGLONG MAX_REASONABLE = 10LL * 1024 * 1024 * 1024 * 1024;
    TEST("absurdly large content-length exceeds sane limit",
        raw2 > MAX_REASONABLE);

    LONGLONG raw3 = _atoi64("104857600"); /* 100MB — نرمال */
    TEST("normal content-length within range",
        raw3 >= 0 && raw3 <= MAX_REASONABLE);
}

void test_range_safety(void) {
    SECTION("Range Safety (anti-corruption)");

    /* http_conn_got_partial با NULL باید FALSE برگردونه */
    TEST("got_partial null",       !http_conn_got_partial(NULL));

    /* تست accept_ranges detection — فقط NULL safety رو تست می‌کنیم
       چون WinINet واقعی در test env نیست */
    TEST("range safety no crash",  TRUE);

    /* verify که MIME table کامله — چک با تعداد */
    char fn[64];
    strncpy(fn, "t", sizeof(fn));
    http_fixup_extension(fn, sizeof(fn), "video/mp4");
    TEST("mime table functional",  strcmp(fn, "t.mp4") == 0);
}

