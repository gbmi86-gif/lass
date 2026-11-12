/* hls: HLS tests (tests/test_hls.c) */
#include "test_common.h"

/* ======================================================
   تست ۱۱: HLS Plugin
   ====================================================== */
void test_hls(void) {
    SECTION("HLS / M3U8 Plugin");

    /* تشخیص URL */
    TEST("m3u8 url detect",       hls_is_m3u8_url("http://cdn.example.com/stream.m3u8"));
    TEST("m3u8 url uppercase",    hls_is_m3u8_url("http://example.com/live.M3U8"));
    TEST("m3u8 query param",      hls_is_m3u8_url("http://example.com/play?type=m3u8"));
    TEST("mp4 not m3u8",          !hls_is_m3u8_url("http://example.com/video.mp4"));
    TEST("null not m3u8",         !hls_is_m3u8_url(NULL));

    /* تشخیص از content */
    const char *m3u8_content = "#EXTM3U\n#EXT-X-VERSION:3\n";
    TEST("content detect valid",  hls_is_m3u8_content(m3u8_content,
                                   (int)strlen(m3u8_content)));
    TEST("content detect short",  !hls_is_m3u8_content("#EXT", 4));
    TEST("content detect null",   !hls_is_m3u8_content(NULL, 0));

    /* HLS_Playlist روی heap — روی stack 8MB میشه که stack overflow میده */
    HLS_Playlist *pl = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    TEST("hls_playlist alloc",    pl != NULL);
    if (!pl) return;

    /* شبیه‌سازی media playlist — بدون شبکه */
    pl->is_master     = FALSE;
    pl->is_endlist    = TRUE;
    pl->target_duration = 10;
    for (int i = 0; i < 3; i++) {
        snprintf(pl->segments[i].url, sizeof(pl->segments[i].url),
                 "http://media.example.com/seg%03d.ts", i+1);
        pl->segments[i].duration = (i < 2) ? 9.009 : 3.003;
        pl->segments[i].index = i;
    }
    pl->segment_count  = 3;
    pl->total_duration = 21.021;

    TEST("segment count",         pl->segment_count == 3);
    TEST("total duration",        pl->total_duration > 20.0 && pl->total_duration < 22.0);
    TEST("is endlist",            pl->is_endlist);
    TEST("not master",            !pl->is_master);
    TEST("first seg url",         strstr(pl->segments[0].url, "seg001") != NULL);
    TEST("last seg duration",     pl->segments[2].duration < 5.0);
    TEST("target duration",       pl->target_duration == 10);

    /* master playlist — هم روی heap */
    HLS_Playlist *master = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    TEST("master alloc",          master != NULL);
    if (!master) { free(pl); return; }

    master->is_master = TRUE;
    master->variants[0].bandwidth = 500000;
    master->variants[1].bandwidth = 1280000;
    master->variants[2].bandwidth = 2560000;
    master->variant_count = 3;
    master->best_variant  = 2;
    snprintf(master->variants[2].url, sizeof(master->variants[2].url),
             "http://example.com/1080p.m3u8");

    TEST("variant count",         master->variant_count == 3);
    TEST("best variant index",    master->best_variant == 2);
    TEST("best variant bw",       master->variants[master->best_variant].bandwidth == 2560000);
    TEST("best variant url",      strstr(master->variants[master->best_variant].url, "1080p") != NULL);

    free(pl);
    free(master);

    /* plugin lifecycle */
    IDM_PluginHooks *plug = hls_plugin_create();
    TEST("hls plugin create",     plug != NULL);
    if (!plug) return;
    TEST("hls name set",          plug->name != NULL);
    TEST("hls api_version match", plug->api_version == IDM_PLUGIN_API_VERSION);

    hls_plugin_destroy(plug);
    TEST("hls destroy",           TRUE);
    hls_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);
}

/* ======================================================
   تست: HLS — تجزیه‌گر، دانلود کامل، رمزگشایی، resume، توقف، و رد موارد ناپشتیبانی
   ====================================================== */
#define HLS_T_BASE "http://127.0.0.1:18480"

#define HLS_T_SEG  150000u

static const uint8_t HLS_TEST_AES_KEY[16] = {
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
};

static const uint8_t HLS_TEST_AES_CT_A[96] = {
    0x49, 0x33, 0x47, 0xa2, 0x3d, 0xb0, 0x12, 0x07, 0x54, 0x9e, 0x99, 0x5d, 0x68, 0xbc, 0x60, 0x11,
    0xd4, 0x81, 0x87, 0xca, 0x6a, 0x21, 0x9c, 0x83, 0xd2, 0xcd, 0xbb, 0x6e, 0xf4, 0x5f, 0x64, 0x61,
    0x5e, 0xb3, 0x94, 0xd8, 0x41, 0xf3, 0xc9, 0xf7, 0xeb, 0xc5, 0xe3, 0x4a, 0x5c, 0xa5, 0x58, 0x47,
    0x83, 0xf1, 0x79, 0x0c, 0x6f, 0x1b, 0x1a, 0x40, 0xde, 0xf3, 0xdf, 0xc3, 0xf7, 0xbc, 0xa0, 0x9e,
    0x40, 0x5f, 0xb0, 0x28, 0x34, 0xfd, 0x89, 0x98, 0x7e, 0x81, 0xd5, 0x72, 0x05, 0xa3, 0xf5, 0xc3,
    0xfd, 0xcc, 0x7b, 0x15, 0x1e, 0xde, 0xf3, 0xcb, 0xa7, 0x91, 0xf1, 0x93, 0xab, 0x0e, 0xdc, 0xdc,
};

static const uint8_t HLS_TEST_AES_CT_B[80] = {
    0x96, 0x6c, 0xbb, 0xd9, 0x90, 0x37, 0x1f, 0xb2, 0xda, 0x65, 0x1f, 0x2f, 0xfa, 0xef, 0x1c, 0x43,
    0x0d, 0x80, 0x6f, 0xf7, 0xa3, 0x43, 0xa1, 0xda, 0x9b, 0xa8, 0x1c, 0x55, 0x66, 0xca, 0x82, 0x3b,
    0x30, 0xea, 0xc6, 0x89, 0xfa, 0x37, 0x88, 0x4f, 0x5e, 0x7c, 0x72, 0xbd, 0x25, 0xe2, 0x06, 0x92,
    0xef, 0x63, 0xaf, 0x24, 0xab, 0x28, 0x5d, 0x42, 0x7e, 0xcc, 0xd2, 0xf5, 0xa9, 0xe5, 0xd6, 0x1c,
    0xfb, 0xc6, 0xcd, 0xb7, 0x5a, 0x93, 0xa2, 0x74, 0x9a, 0xe1, 0x3a, 0x89, 0xf6, 0x5a, 0x51, 0x6c,
};

static uint8_t g_hls_plain[4][HLS_T_SEG];

static uint8_t g_hls_fmp4_init[300];

static uint8_t g_hls_fmp4_seg[2][120000];

static uint8_t g_hls_big[3000];

static uint8_t g_hls_res[5][80000];

static uint8_t g_hls_stop[4][60000];

static uint8_t g_hls_eng[2][50000];

static void hls_fill(uint8_t *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)((i * 131u + seed * 17u + (i >> 8)) & 0xFFu);
}

static HlsRun hls_run_for(const char *url, const char *tmp, const char *out) {
    HlsRun r;
    memset(&r, 0, sizeof(r));
    r.url = url;
    r.temp_path = tmp;
    r.out_path = out;
    return r;
}

static BOOL hls_output_equals(const char *path, const uint8_t *want, size_t want_len) {
    uint8_t *got = NULL;
    size_t gl = 0;
    BOOL ok = test_read_file(path, &got, &gl) && gl == want_len &&
              memcmp(got, want, want_len) == 0;
    free(got);
    return ok;
}

static BOOL hls_container_has(const char *tmp, const char *name) {
    TelsContainer *tc = tels_open(tmp, FALSE);
    if (!tc) return FALSE;
    TelsId id = 0;
    BOOL found = tels_lookup(tc, TELS_ROOT, name, &id);
    tels_close(tc);
    return found;
}

struct HlsTestProbe {
    uint32_t stop_at;
    BOOL     stop;
};

static void hls_test_progress(uint64_t bytes, uint32_t done, uint32_t total, void *ctx) {
    struct HlsTestProbe *pr = (struct HlsTestProbe *)ctx;
    (void)bytes;
    (void)total;
    if (done >= pr->stop_at) pr->stop = TRUE;
}

static int hls_test_should_stop(void *ctx) {
    return ((struct HlsTestProbe *)ctx)->stop ? 1 : 0;
}

void test_hls_parser(void) {
    SECTION("HLS parser: keys, maps, byte ranges, sequence numbers, unsupported features");
    static const char MEDIA[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/k.bin\",IV=0x000102030405060708090a0b0c0d0e0f\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:4.0,\n"
        "a.m4s\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/k.bin\"\n"
        "#EXTINF:2.5,\n"
        "#EXT-X-BYTERANGE:500@100\n"
        "b.ts\n"
        "#EXTINF:2.5,\n"
        "#EXT-X-BYTERANGE:300\n"
        "b.ts\n"
        "#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:1.0,\n"
        "c.ts\n"
        "#EXT-X-ENDLIST\n";
    HLS_Playlist *pl = (HLS_Playlist *)calloc(1, sizeof(HLS_Playlist));
    TEST("parser allocates a playlist", pl != NULL);
    if (!pl) return;
    IDM_Result r = hls_parse_text("https://cdn.example.com/vid/index.m3u8",
                                  MEDIA, (int)(sizeof(MEDIA) - 1), pl);
    TEST("media playlist parses", r == IDM_OK && !pl->is_master);
    TEST("four segments found", pl->segment_count == 4);
    TEST("ENDLIST recognised", pl->is_endlist);
    TEST("media sequence read", pl->media_sequence == 7);
    TEST("two key entries: explicit IV and default IV", pl->key_count == 2 &&
         pl->keys[0].has_iv && !pl->keys[1].has_iv);
    TEST("relative key URI resolved against the playlist",
         strcmp(pl->keys[0].uri, "https://cdn.example.com/vid/keys/k.bin") == 0);
    TEST("explicit IV bytes read", pl->keys[0].iv[0] == 0x00 &&
         pl->keys[0].iv[1] == 0x01 && pl->keys[0].iv[15] == 0x0f);
    TEST("first segment uses the explicit key", pl->segments[0].key_index == 0);
    TEST("second segment uses the default-IV key", pl->segments[1].key_index == 1);
    TEST("last segment is clear (METHOD=NONE)", pl->segments[3].key_index == -1);
    TEST("init section resolved and used", pl->map_count == 1 &&
         strstr(pl->maps[0].uri, "/vid/init.mp4") != NULL && pl->segments[0].map_index == 0);
    TEST("default IV follows the media sequence",
         pl->segments[0].media_seq == 7 && pl->segments[2].media_seq == 9);
    TEST("byte range with an offset parsed",
         pl->segments[1].has_range && pl->segments[1].range_len == 500 &&
         pl->segments[1].range_off == 100);
    TEST("byte range without an offset continues the same resource",
         pl->segments[2].has_range && pl->segments[2].range_len == 300 &&
         pl->segments[2].range_off == 600);
    TEST("no unsupported feature in a valid AES playlist", pl->unsupported[0] == '\0');

    static const char DRM[] =
        "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k.bin\"\n#EXTINF:4.0,\nd0.ts\n#EXT-X-ENDLIST\n";
    memset(pl, 0, sizeof(*pl));
    hls_parse_text("http://h.example/p/i.m3u8", DRM, (int)(sizeof(DRM) - 1), pl);
    TEST("SAMPLE-AES is reported as unsupported", strstr(pl->unsupported, "SAMPLE-AES") != NULL);

    static const char MASTER[] =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,CODECS=\"avc1.4d401e\"\n"
        "low/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,AUDIO=\"aud\"\n"
        "high/index.m3u8\n";
    memset(pl, 0, sizeof(*pl));
    hls_parse_text("http://h.example/p/master.m3u8", MASTER, (int)(sizeof(MASTER) - 1), pl);
    TEST("master playlist: both variants found", pl->is_master && pl->variant_count == 2);
    TEST("master playlist: the highest bandwidth is the best variant",
         pl->best_variant == 1 && pl->variants[1].width == 1280);
    TEST("master playlist: variant URL resolved",
         strcmp(pl->variants[1].url, "http://h.example/p/high/index.m3u8") == 0);
    TEST("master playlist: separate audio group detected",
         pl->has_separate_audio && pl->variants[1].audio_group[0] != '\0');

    static const char LIVE[] = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nl0.ts\n";
    memset(pl, 0, sizeof(*pl));
    hls_parse_text("http://h.example/p/live.m3u8", LIVE, (int)(sizeof(LIVE) - 1), pl);
    TEST("live playlist: ENDLIST absent", !pl->is_endlist && pl->segment_count == 1);

    static const char PAGE[] = "<html><body>no playlist here</body></html>";
    TEST("a page without #EXTM3U is rejected",
         hls_parse_text("http://h.example/x", PAGE, (int)(sizeof(PAGE) - 1), pl) == IDM_ERR_INVALID);
    free(pl);
}

void test_hls_plain(void) {
    SECTION("HLS plain stream: four segments are joined into one .ts file");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:4.0,\nseg0.ts\n#EXTINF:4.0,\nseg1.ts\n#EXTINF:4.0,\nseg2.ts\n"
        "#EXTINF:4.0,\nseg3.ts\n#EXT-X-ENDLIST\n";
    static uint8_t want[4u * HLS_T_SEG];
    hls_test_server_reset_hits();
    hls_test_server_set("/plain/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    for (uint32_t i = 0; i < 4u; i++) {
        char path[48];
        hls_fill(g_hls_plain[i], HLS_T_SEG, i + 1u);
        snprintf(path, sizeof(path), "/plain/seg%u.ts", (unsigned)i);
        hls_test_server_set(path, g_hls_plain[i], HLS_T_SEG);
        memcpy(want + (size_t)i * HLS_T_SEG, g_hls_plain[i], HLS_T_SEG);
    }
    DeleteFileA("tels_test_hls_plain.tels");
    DeleteFileA("tels_test_hls_plain.ts");
    HlsRun run = hls_run_for(HLS_T_BASE "/plain/index.m3u8",
                             "tels_test_hls_plain.tels", "tels_test_hls_plain.m3u8");
    HlsStatus st = hls_download_run(&run);
    TEST("plain stream downloads completely", st == HLS_OK);
    TEST("final file takes the .ts extension",
         st == HLS_OK && strstr(run.final_path, ".ts") != NULL);
    TEST("output equals the four segments joined",
         st == HLS_OK && hls_output_equals(run.final_path, want, sizeof(want)));
    TEST("temporary file is removed after completion",
         GetFileAttributesA("tels_test_hls_plain.tels") == INVALID_FILE_ATTRIBUTES);
    TEST("each segment is fetched exactly once",
         hls_test_server_hits("/plain/seg0.ts") == 1 && hls_test_server_hits("/plain/seg3.ts") == 1);
    DeleteFileA(run.final_path);
}

void test_hls_aes(void) {
    SECTION("HLS AES-128: segments decrypt with an explicit IV and with the default IV");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV=0x000102030405060708090a0b0c0d0e0f\n"
        "#EXTINF:4.0,\nseg_a.ts\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:4.0,\nseg_b.ts\n"
        "#EXT-X-ENDLIST\n";
    static const char PT_A[] =
        "AES segment A: explicit IV, decrypted by the HLS downloader. Lass test 0123456789.";
    static const char PT_B[] =
        "AES segment B: default IV from the media sequence number (8). Lass test ABCDEF.";
    static uint8_t want[(sizeof(PT_A) - 1) + (sizeof(PT_B) - 1)];
    memcpy(want, PT_A, sizeof(PT_A) - 1);
    memcpy(want + (sizeof(PT_A) - 1), PT_B, sizeof(PT_B) - 1);
    hls_test_server_reset_hits();
    hls_test_server_set("/aes/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    hls_test_server_set("/aes/key.bin", HLS_TEST_AES_KEY, (uint32_t)sizeof(HLS_TEST_AES_KEY));
    hls_test_server_set("/aes/seg_a.ts", HLS_TEST_AES_CT_A, (uint32_t)sizeof(HLS_TEST_AES_CT_A));
    hls_test_server_set("/aes/seg_b.ts", HLS_TEST_AES_CT_B, (uint32_t)sizeof(HLS_TEST_AES_CT_B));
    DeleteFileA("tels_test_hls_aes.tels");
    DeleteFileA("tels_test_hls_aes.ts");
    HlsRun run = hls_run_for(HLS_T_BASE "/aes/index.m3u8",
                             "tels_test_hls_aes.tels", "tels_test_hls_aes.m3u8");
    HlsStatus st = hls_download_run(&run);
    TEST("AES stream downloads and decrypts", st == HLS_OK);
    TEST("decrypted output equals both plaintexts joined",
         st == HLS_OK && hls_output_equals(run.final_path, want, sizeof(want)));
    TEST("each key entry is fetched once (two entries, one URI)",
         hls_test_server_hits("/aes/key.bin") == 2);
    DeleteFileA(run.final_path);
}

void test_hls_fmp4(void) {
    SECTION("HLS fMP4: the init section comes first and the output is an .mp4 file");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:4.0,\nseg0.m4s\n#EXTINF:4.0,\nseg1.m4s\n#EXT-X-ENDLIST\n";
    static uint8_t want[300u + 120000u + 90000u];
    hls_fill(g_hls_fmp4_init, sizeof(g_hls_fmp4_init), 9u);
    hls_fill(g_hls_fmp4_seg[0], 120000u, 1u);
    hls_fill(g_hls_fmp4_seg[1], 90000u, 2u);
    memcpy(want, g_hls_fmp4_init, 300u);
    memcpy(want + 300u, g_hls_fmp4_seg[0], 120000u);
    memcpy(want + 300u + 120000u, g_hls_fmp4_seg[1], 90000u);
    hls_test_server_reset_hits();
    hls_test_server_set("/fmp4/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    hls_test_server_set("/fmp4/init.mp4", g_hls_fmp4_init, 300u);
    hls_test_server_set("/fmp4/seg0.m4s", g_hls_fmp4_seg[0], 120000u);
    hls_test_server_set("/fmp4/seg1.m4s", g_hls_fmp4_seg[1], 90000u);
    DeleteFileA("tels_test_hls_fmp4.tels");
    DeleteFileA("tels_test_hls_fmp4.mp4");
    HlsRun run = hls_run_for(HLS_T_BASE "/fmp4/index.m3u8",
                             "tels_test_hls_fmp4.tels", "tels_test_hls_fmp4.m3u8");
    HlsStatus st = hls_download_run(&run);
    TEST("fMP4 stream downloads completely", st == HLS_OK);
    TEST("final file takes the .mp4 extension",
         st == HLS_OK && strstr(run.final_path, ".mp4") != NULL);
    TEST("output is init section followed by both segments",
         st == HLS_OK && hls_output_equals(run.final_path, want, sizeof(want)));
    TEST("init section is fetched exactly once", hls_test_server_hits("/fmp4/init.mp4") == 1);
    DeleteFileA(run.final_path);
}

void test_hls_byterange(void) {
    SECTION("HLS byte ranges: three segments are slices of one resource");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXTINF:2.0,\n#EXT-X-BYTERANGE:1000\nbig.ts\n"
        "#EXTINF:2.0,\n#EXT-X-BYTERANGE:1000@1000\nbig.ts\n"
        "#EXTINF:2.0,\n#EXT-X-BYTERANGE:1000@2000\nbig.ts\n"
        "#EXT-X-ENDLIST\n";
    hls_fill(g_hls_big, sizeof(g_hls_big), 5u);
    hls_test_server_reset_hits();
    hls_test_server_set("/range/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    hls_test_server_set("/range/big.ts", g_hls_big, (uint32_t)sizeof(g_hls_big));
    DeleteFileA("tels_test_hls_range.tels");
    DeleteFileA("tels_test_hls_range.ts");
    HlsRun run = hls_run_for(HLS_T_BASE "/range/index.m3u8",
                             "tels_test_hls_range.tels", "tels_test_hls_range.m3u8");
    HlsStatus st = hls_download_run(&run);
    TEST("byte-range stream downloads", st == HLS_OK);
    TEST("output equals the resource (three 1000-byte slices)",
         st == HLS_OK && hls_output_equals(run.final_path, g_hls_big, sizeof(g_hls_big)));
    TEST("the resource is requested once per slice", hls_test_server_hits("/range/big.ts") == 3);
    DeleteFileA(run.final_path);
}

void test_hls_resume(void) {
    SECTION("HLS resume: a failed segment does not discard the finished ones");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXTINF:4.0,\nseg0.ts\n#EXTINF:4.0,\nseg1.ts\n#EXTINF:4.0,\nseg2.ts\n"
        "#EXTINF:4.0,\nseg3.ts\n#EXTINF:4.0,\nseg4.ts\n#EXT-X-ENDLIST\n";
    static uint8_t want[5u * 80000u];
    hls_test_server_set("/resume/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    for (uint32_t i = 0; i < 5u; i++) {
        char path[48];
        hls_fill(g_hls_res[i], 80000u, i + 11u);
        snprintf(path, sizeof(path), "/resume/seg%u.ts", (unsigned)i);
        hls_test_server_set(path, g_hls_res[i], 80000u);
        memcpy(want + (size_t)i * 80000u, g_hls_res[i], 80000u);
    }
    hls_test_server_fail("/resume/seg2.ts", 404, 0);
    DeleteFileA("tels_test_hls_resume.tels");
    DeleteFileA("tels_test_hls_resume.ts");
    hls_test_server_reset_hits();

    HlsRun a = hls_run_for(HLS_T_BASE "/resume/index.m3u8",
                           "tels_test_hls_resume.tels", "tels_test_hls_resume.m3u8");
    HlsStatus sa = hls_download_run(&a);
    TEST("a missing segment fails the run as not found", sa == HLS_ERR_NOT_FOUND);
    TEST("the failure names the segment", strstr(a.error, "segment #3") != NULL);
    TEST("a 404 is not retried", hls_test_server_hits("/resume/seg2.ts") == 1);
    TEST("segments after the failure are not fetched yet", hls_test_server_hits("/resume/seg3.ts") == 0);
    TEST("finished segments are kept in the temporary file",
         hls_container_has("tels_test_hls_resume.tels", "d000000") &&
         hls_container_has("tels_test_hls_resume.tels", "d000001") &&
         !hls_container_has("tels_test_hls_resume.tels", "d000002"));

    hls_test_server_clear_fail("/resume/seg2.ts");
    hls_test_server_reset_hits();
    HlsRun b = hls_run_for(HLS_T_BASE "/resume/index.m3u8",
                           "tels_test_hls_resume.tels", "tels_test_hls_resume.m3u8");
    HlsStatus sb = hls_download_run(&b);
    TEST("the next run completes the stream", sb == HLS_OK);
    TEST("finished segments are not fetched again",
         hls_test_server_hits("/resume/seg0.ts") == 0 && hls_test_server_hits("/resume/seg1.ts") == 0);
    TEST("remaining segments are fetched exactly once",
         hls_test_server_hits("/resume/seg2.ts") == 1 && hls_test_server_hits("/resume/seg4.ts") == 1);
    TEST("output equals the five segments joined",
         sb == HLS_OK && hls_output_equals(b.final_path, want, sizeof(want)));
    TEST("temporary file is removed after completion",
         GetFileAttributesA("tels_test_hls_resume.tels") == INVALID_FILE_ATTRIBUTES);
    DeleteFileA(b.final_path);
}

void test_hls_stop_resume(void) {
    SECTION("HLS pause: a stopped run keeps finished segments and the next run continues");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXTINF:4.0,\nq0.ts\n#EXTINF:4.0,\nq1.ts\n#EXTINF:4.0,\nq2.ts\n#EXTINF:4.0,\nq3.ts\n"
        "#EXT-X-ENDLIST\n";
    static uint8_t want[4u * 60000u];
    hls_test_server_set("/stop/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    for (uint32_t i = 0; i < 4u; i++) {
        char path[48];
        hls_fill(g_hls_stop[i], 60000u, i + 31u);
        snprintf(path, sizeof(path), "/stop/q%u.ts", (unsigned)i);
        hls_test_server_set(path, g_hls_stop[i], 60000u);
        memcpy(want + (size_t)i * 60000u, g_hls_stop[i], 60000u);
    }
    DeleteFileA("tels_test_hls_stop.tels");
    DeleteFileA("tels_test_hls_stop.ts");
    hls_test_server_reset_hits();

    struct HlsTestProbe pr;
    pr.stop_at = 2u;
    pr.stop = FALSE;
    HlsRun a = hls_run_for(HLS_T_BASE "/stop/index.m3u8",
                           "tels_test_hls_stop.tels", "tels_test_hls_stop.m3u8");
    a.should_stop = hls_test_should_stop;
    a.stop_ctx = &pr;
    a.on_progress = hls_test_progress;
    a.progress_ctx = &pr;
    HlsStatus sa = hls_download_run(&a);
    TEST("stopped run reports STOPPED", sa == HLS_ERR_STOPPED);
    TEST("stopped run fetched only the first two segments",
         hls_test_server_hits("/stop/q0.ts") == 1 && hls_test_server_hits("/stop/q1.ts") == 1 &&
         hls_test_server_hits("/stop/q2.ts") == 0);
    TEST("stopped run keeps its finished segments",
         hls_container_has("tels_test_hls_stop.tels", "d000000") &&
         hls_container_has("tels_test_hls_stop.tels", "d000001") &&
         !hls_container_has("tels_test_hls_stop.tels", "d000002"));
    TEST("stopped run leaves no output file yet",
         GetFileAttributesA("tels_test_hls_stop.ts") == INVALID_FILE_ATTRIBUTES);

    hls_test_server_reset_hits();
    HlsRun b = hls_run_for(HLS_T_BASE "/stop/index.m3u8",
                           "tels_test_hls_stop.tels", "tels_test_hls_stop.m3u8");
    HlsStatus sb = hls_download_run(&b);
    TEST("the next run completes the stream", sb == HLS_OK);
    TEST("finished segments are not fetched again",
         hls_test_server_hits("/stop/q0.ts") == 0 && hls_test_server_hits("/stop/q1.ts") == 0);
    TEST("remaining segments are fetched exactly once",
         hls_test_server_hits("/stop/q2.ts") == 1 && hls_test_server_hits("/stop/q3.ts") == 1);
    TEST("output equals the four segments joined",
         sb == HLS_OK && hls_output_equals(b.final_path, want, sizeof(want)));
    DeleteFileA(b.final_path);
}

void test_hls_rejects(void) {
    SECTION("HLS rejects what it cannot download correctly: live playlists, DRM, non-HLS pages");
    static const char LIVE[] = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nl0.ts\n";
    static const char DRM[] =
        "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k.bin\"\n#EXTINF:4.0,\nd0.ts\n#EXT-X-ENDLIST\n";
    static const char PAGE[] = "<html><body>not a playlist</body></html>";
    hls_test_server_set("/rej/live.m3u8", (const uint8_t *)LIVE, (uint32_t)(sizeof(LIVE) - 1));
    hls_test_server_set("/rej/drm.m3u8", (const uint8_t *)DRM, (uint32_t)(sizeof(DRM) - 1));
    hls_test_server_set("/rej/page.m3u8", (const uint8_t *)PAGE, (uint32_t)(sizeof(PAGE) - 1));
    DeleteFileA("tels_test_hls_live.tels");
    DeleteFileA("tels_test_hls_live.m3u8");

    HlsRun a = hls_run_for(HLS_T_BASE "/rej/live.m3u8",
                           "tels_test_hls_live.tels", "tels_test_hls_live.m3u8");
    HlsStatus sa = hls_download_run(&a);
    TEST("live playlist is refused as unsupported",
         sa == HLS_ERR_UNSUPPORTED && strstr(a.error, "live") != NULL);
    TEST("live playlist leaves no temporary file",
         GetFileAttributesA("tels_test_hls_live.tels") == INVALID_FILE_ATTRIBUTES);

    HlsRun b = hls_run_for(HLS_T_BASE "/rej/drm.m3u8",
                           "tels_test_hls_drm.tels", "tels_test_hls_drm.m3u8");
    HlsStatus sb = hls_download_run(&b);
    TEST("SAMPLE-AES is refused as unsupported",
         sb == HLS_ERR_UNSUPPORTED && strstr(b.error, "SAMPLE-AES") != NULL);

    HlsRun c = hls_run_for(HLS_T_BASE "/rej/page.m3u8",
                           "tels_test_hls_page.tels", "tels_test_hls_page.m3u8");
    TEST("a non-HLS page is a parse error", hls_download_run(&c) == HLS_ERR_PARSE);

    HlsRun d = hls_run_for(HLS_T_BASE "/rej/missing.m3u8",
                           "tels_test_hls_missing.tels", "tels_test_hls_missing.m3u8");
    TEST("a missing playlist is reported as not found", hls_download_run(&d) == HLS_ERR_NOT_FOUND);
}

void test_hls_engine(void) {
    SECTION("HLS through the engine: a .m3u8 link is queued, downloaded and completed");
    static const char PL[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\ne0.ts\n#EXTINF:4.0,\ne1.ts\n"
        "#EXT-X-ENDLIST\n";
    static uint8_t want[2u * 50000u];
    hls_fill(g_hls_eng[0], 50000u, 21u);
    hls_fill(g_hls_eng[1], 50000u, 22u);
    memcpy(want, g_hls_eng[0], 50000u);
    memcpy(want + 50000u, g_hls_eng[1], 50000u);
    hls_test_server_set("/eng/index.m3u8", (const uint8_t *)PL, (uint32_t)(sizeof(PL) - 1));
    hls_test_server_set("/eng/e0.ts", g_hls_eng[0], 50000u);
    hls_test_server_set("/eng/e1.ts", g_hls_eng[1], 50000u);
    DeleteFileA("tels_test_hls_eng.ts");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine created for HLS", eng != NULL);
    if (!eng) return;
    IDM_Download *dl = engine_add(eng, HLS_T_BASE "/eng/index.m3u8",
                                  "tels_test_hls_eng.m3u8", 1u, 0u);
    TEST("HLS link queued in the engine", dl != NULL);
    uint32_t id = dl ? dl->id : 0u;
    DL_State state = DL_IDLE;
    DWORD t0 = GetTickCount();
    while (dl && GetTickCount() - t0 < 20000u) {
        IDM_Download *cur = engine_find(eng, id);
        state = cur ? cur->state : DL_FAILED;
        if (state == DL_COMPLETED || state == DL_FAILED) break;
        Sleep(50);
    }
    TEST("engine completes the HLS download", state == DL_COMPLETED);
    IDM_Download *fin = engine_find(eng, id);
    TEST("completed file holds both segments",
         state == DL_COMPLETED && fin != NULL && hls_output_equals(fin->out_path, want, sizeof(want)));
    TEST("temporary file removed by the engine's HLS path",
         fin != NULL && GetFileAttributesA(fin->temp_path) == INVALID_FILE_ATTRIBUTES);
    engine_destroy(eng);
    if (fin) DeleteFileA(fin->out_path);
}

void test_hls_engine_failure(void) {
    SECTION("HLS through the engine: a refused stream ends as failed, with its reason kept");
    static const char LIVE[] = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nlv0.ts\n";
    hls_test_server_set("/engf/index.m3u8", (const uint8_t *)LIVE, (uint32_t)(sizeof(LIVE) - 1));
    IDM_Engine *eng = engine_create(NULL);
    TEST("engine created for the failing HLS case", eng != NULL);
    if (!eng) return;
    IDM_Download *dl = engine_add(eng, HLS_T_BASE "/engf/index.m3u8",
                                  "tels_test_hls_engf.m3u8", 1u, 0u);
    TEST("refused HLS link queued in the engine", dl != NULL);
    uint32_t id = dl ? dl->id : 0u;
    DL_State state = DL_IDLE;
    DWORD t0 = GetTickCount();
    while (dl && GetTickCount() - t0 < 20000u) {
        IDM_Download *cur = engine_find(eng, id);
        state = cur ? cur->state : DL_FAILED;
        if (state == DL_COMPLETED || state == DL_FAILED) break;
        Sleep(50);
    }
    IDM_Download *fin = engine_find(eng, id);
    TEST("engine marks a live stream as failed", state == DL_FAILED);
    TEST("failure keeps the reason intact (not empty, not cut off)",
         fin != NULL && strstr(fin->fail_reason, "live") != NULL &&
         strlen(fin->fail_reason) > 20);
    TEST("refused stream leaves no temporary file",
         fin != NULL && GetFileAttributesA(fin->temp_path) == INVALID_FILE_ATTRIBUTES);
    engine_destroy(eng);
}

void test_hls_all(void) {
    if (!hls_test_server_start(18480)) {
        TEST("HLS test server listens on loopback", FALSE);
        return;
    }
    test_hls_parser();
    test_hls_plain();
    test_hls_aes();
    test_hls_fmp4();
    test_hls_byterange();
    test_hls_resume();
    test_hls_stop_resume();
    test_hls_rejects();
    test_hls_engine();
    test_hls_engine_failure();
    hls_test_server_stop();
}

