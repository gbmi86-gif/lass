/* test_media.c — رفتار HTTP مشترک (media_http) روی سرور loopback */
#include "test_common.h"
#include "media_http.h"
#include "test_hls_server.h"

#define MT_PORT   18499
#define MT_BASE   "http://127.0.0.1:18499"
#define MT_CLOSED "http://127.0.0.1:18597/x"   /* هیچ سروری روی این پورت نیست */

static int mt_calls = 0;

static int mt_stop_now(void *ctx) {
    (void)ctx;
    return 1;
}

/* پرسش اول FALSE می‌دهد؛ از دومی به بعد TRUE. توقف دقیقاً وسط انتظار رخ می‌دهد. */
static int mt_stop_during_wait(void *ctx) {
    (void)ctx;
    return ++mt_calls > 1;
}

static MediaHttp mt_view(void) {
    MediaHttp h;
    memset(&h, 0, sizeof(h));
    return h;
}

void test_media_http(void) {
    uint8_t body[256];
    MediaBuf b;
    MediaHttp h;
    char err[160];
    MediaResult r;
    DWORD t0, elapsed;
    uint32_t i;

    SECTION("Shared HTTP fetch (media_http): success, range, status rules, retry, size limit, stop");
    for (i = 0; i < sizeof(body); i++) body[i] = (uint8_t)(i * 7u + 3u);
    memset(&b, 0, sizeof(b));
    if (!hls_test_server_start(MT_PORT)) {
        TEST("test server starts", FALSE);
        return;
    }
    hls_test_server_set("/ok.bin", body, sizeof(body));
    hls_test_server_set("/flaky.bin", body, sizeof(body));
    hls_test_server_set("/dead.bin", body, sizeof(body));
    hls_test_server_set("/broken.bin", body, sizeof(body));
    hls_test_server_set("/ra.bin", body, sizeof(body));
    hls_test_server_set("/c408.bin", body, sizeof(body));
    hls_test_server_set("/c403.bin", body, sizeof(body));
    hls_test_server_set("/c400.bin", body, sizeof(body));
    hls_test_server_set("/norange.bin", body, sizeof(body));
    hls_test_server_set("/stop.bin", body, sizeof(body));
    hls_test_server_fail("/flaky.bin", 503, 1);
    hls_test_server_fail("/dead.bin", 404, 0);
    hls_test_server_fail("/broken.bin", 500, 0);
    hls_test_server_fail_retry_after("/ra.bin", 429, 1, 2);
    hls_test_server_fail("/c408.bin", 408, 1);
    hls_test_server_fail("/c403.bin", 403, 1);
    hls_test_server_fail("/c400.bin", 400, 0);
    hls_test_server_ignore_range("/norange.bin");
    hls_test_server_fail("/stop.bin", 503, 0);
    hls_test_server_reset_hits();
    h = mt_view();

    r = media_fetch(&h, MT_BASE "/ok.bin", 0, 0, 4096, FALSE, 3, &b, "ok", err, sizeof(err));
    TEST("a plain GET returns the whole body",
         r == MEDIA_OK && b.len == sizeof(body) && memcmp(b.data, body, sizeof(body)) == 0);
    TEST("a successful GET is requested once", hls_test_server_hits("/ok.bin") == 1);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/ok.bin", 16, 31, 16, TRUE, 3, &b, "range", err, sizeof(err));
    TEST("a byte range returns exactly that slice",
         r == MEDIA_OK && b.len == 16 && memcmp(b.data, body + 16, 16) == 0);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/dead.bin", 0, 0, 4096, FALSE, 3, &b, "dead", err, sizeof(err));
    TEST("404 is permanent and not retried",
         r == MEDIA_NOT_FOUND && hls_test_server_hits("/dead.bin") == 1);
    TEST("the 404 message names the resource and the status", strstr(err, "dead: HTTP 404") != NULL);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/flaky.bin", 0, 0, 4096, FALSE, 3, &b, "flaky", err, sizeof(err));
    TEST("a 503 that clears is retried and then succeeds", r == MEDIA_OK && b.len == sizeof(body));
    TEST("the retried request was sent twice", hls_test_server_hits("/flaky.bin") == 2);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/broken.bin", 0, 0, 4096, FALSE, 2, &b, "broken", err, sizeof(err));
    TEST("a persistent 500 fails after the given attempts",
         r == MEDIA_FAILED && hls_test_server_hits("/broken.bin") == 2);
    TEST("the failure says how many attempts were made", strstr(err, "HTTP 500 after 2 attempts") != NULL);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/ok.bin", 0, 0, 100, FALSE, 3, &b, "big", err, sizeof(err));
    TEST("a body over the limit is refused",
         r == MEDIA_FAILED && strstr(err, "larger than the supported size") != NULL);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    t0 = GetTickCount();
    r = media_fetch(&h, MT_BASE "/ra.bin", 0, 0, 4096, FALSE, 2, &b, "ra", err, sizeof(err));
    elapsed = GetTickCount() - t0;
    TEST("a 429 with Retry-After waits the server's delay (2 s, not 1 s)",
         r == MEDIA_OK && elapsed >= 1900);
    TEST("the Retry-After retry is the second request", hls_test_server_hits("/ra.bin") == 2);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/c408.bin", 0, 0, 4096, FALSE, 3, &b, "c408", err, sizeof(err));
    TEST("408 is transient: retried and then succeeds",
         r == MEDIA_OK && hls_test_server_hits("/c408.bin") == 2);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/c403.bin", 0, 0, 4096, FALSE, 3, &b, "c403", err, sizeof(err));
    TEST("403 is transient (rate limiting): retried and then succeeds",
         r == MEDIA_OK && hls_test_server_hits("/c403.bin") == 2);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/c400.bin", 0, 0, 4096, FALSE, 3, &b, "c400", err, sizeof(err));
    TEST("400 is permanent and not retried",
         r == MEDIA_NOT_FOUND && hls_test_server_hits("/c400.bin") == 1 && strstr(err, "c400: HTTP 400") != NULL);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    t0 = GetTickCount();
    r = media_fetch(&h, MT_CLOSED, 0, 0, 4096, FALSE, 2, &b, "closed", err, sizeof(err));
    elapsed = GetTickCount() - t0;
    TEST("a refused connection is retried, then reported as a network error",
         r == MEDIA_FAILED && strstr(err, "network error after 2 attempts") != NULL && elapsed >= 900);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    r = media_fetch(&h, MT_BASE "/norange.bin", 16, 31, 16, TRUE, 3, &b, "norange", err, sizeof(err));
    TEST("a server that ignores Range fails the fetch without a retry",
         r == MEDIA_FAILED && strstr(err, "ignored the byte range request") != NULL &&
         hls_test_server_hits("/norange.bin") == 1);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    h.should_stop = mt_stop_now;
    r = media_fetch(&h, MT_BASE "/ok.bin", 0, 0, 4096, FALSE, 3, &b, "stopped", err, sizeof(err));
    TEST("a stop request before the attempt ends the fetch as stopped", r == MEDIA_STOPPED);
    media_buf_free(&b);

    memset(&b, 0, sizeof(b));
    mt_calls = 0;
    h.should_stop = mt_stop_during_wait;
    r = media_fetch(&h, MT_BASE "/stop.bin", 0, 0, 4096, FALSE, 3, &b, "stop", err, sizeof(err));
    TEST("a stop request during the wait ends the fetch without another request",
         r == MEDIA_STOPPED && hls_test_server_hits("/stop.bin") == 1);
    media_buf_free(&b);
    h = mt_view();

    TEST("default retry waits double: 1, 2, 4 seconds",
         media_retry_wait_ms(1, 0) == 1000 && media_retry_wait_ms(2, 0) == 2000 &&
         media_retry_wait_ms(3, 0) == 4000);
    TEST("Retry-After replaces the default wait", media_retry_wait_ms(1, 2) == 2000);
    TEST("a wait is capped at 60 seconds",
         media_retry_wait_ms(1, 120) == 60000 && media_retry_wait_ms(9, 0) == 60000);
    TEST("attempt zero is treated as the first attempt", media_retry_wait_ms(0, 0) == 1000);

    hls_test_server_stop();
}
