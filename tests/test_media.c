/* test_media.c — رفتار HTTP مشترک (media_http) روی سرور loopback */
#include "test_common.h"
#include "media_http.h"
#include "test_hls_server.h"

#define MT_PORT 18499
#define MT_BASE "http://127.0.0.1:18499"

static int mt_stop_now(void *ctx) {
    (void)ctx;
    return 1;
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
    uint32_t i;

    SECTION("Shared HTTP fetch (media_http): success, range, status rules, retry, size limit, stop");
    for (i = 0; i < sizeof(body); i++) body[i] = (uint8_t)(i * 7u + 3u);
    memset(&b, 0, sizeof(b));
    if (!hls_test_server_start(MT_PORT)) {
        TEST("test server starts", FALSE);
        return;
    }
    hls_test_server_set("/ok.bin", body, sizeof(body));
    hls_test_server_set("/dead.bin", body, sizeof(body));
    hls_test_server_set("/flaky.bin", body, sizeof(body));
    hls_test_server_set("/broken.bin", body, sizeof(body));
    hls_test_server_fail("/dead.bin", 404, 1000);
    hls_test_server_fail("/flaky.bin", 503, 1);
    hls_test_server_fail("/broken.bin", 500, 1000);
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
    TEST("404 is permanent and not retried", r == MEDIA_NOT_FOUND && hls_test_server_hits("/dead.bin") == 1);
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
    h.should_stop = mt_stop_now;
    r = media_fetch(&h, MT_BASE "/ok.bin", 0, 0, 4096, FALSE, 3, &b, "stopped", err, sizeof(err));
    TEST("a stop request ends the fetch as stopped", r == MEDIA_STOPPED);
    media_buf_free(&b);

    hls_test_server_stop();
}
