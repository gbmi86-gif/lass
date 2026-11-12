/* dash: DASH tests (tests/test_dash.c) */
#include "test_common.h"

/* ======================================================
   تست ۱۸: DASH / MPD Plugin
   ====================================================== */
void test_dash(void) {
    SECTION("DASH / MPD Plugin");

    /* تشخیص URL */
    TEST("mpd url detect",        dash_is_mpd_url("http://cdn.example.com/manifest.mpd"));
    TEST("mpd url uppercase",     dash_is_mpd_url("http://example.com/stream.MPD"));
    TEST("non-mpd rejected",      !dash_is_mpd_url("http://example.com/video.mp4"));
    TEST("null rejected",         !dash_is_mpd_url(NULL));

    /* تشخیص از content */
    const char *mpd_content = "<?xml version=\"1.0\"?><MPD type=\"static\">";
    TEST("content detect valid",  dash_is_mpd_content(mpd_content, (int)strlen(mpd_content)));
    TEST("content detect short",  !dash_is_mpd_content("<MP", 3));
    TEST("content detect null",   !dash_is_mpd_content(NULL, 0));

    /* plugin lifecycle */
    IDM_PluginHooks *plug = dash_plugin_create();
    TEST("dash create",           plug != NULL);
    if (!plug) return;

    TEST("dash name set",         plug->name != NULL);
    TEST("dash version set",      plug->version != NULL);
    TEST("dash api_version match",plug->api_version == IDM_PLUGIN_API_VERSION);

    /* NULL safety */
    dash_is_mpd_url(NULL);
    dash_is_mpd_content(NULL, 0);
    TEST("null safety",           TRUE);

    dash_plugin_destroy(plug);
    TEST("dash destroy",          TRUE);
    dash_plugin_destroy(NULL);
    TEST("destroy null safe",     TRUE);
}

/* ======================================================
   تست ۲۵: DASH — mp4 قطعه‌ای، قواعد manifest، دانلود کامل، resume، توقف، رد صریح
   ====================================================== */
#define DT_BASE "http://127.0.0.1:18490"

#define DT_PORT 18490

typedef struct { uint8_t *d; size_t n, cap; } DtBuf;

static void dt_put(DtBuf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096u;
        uint8_t *nd;
        while (nc < b->n + n) nc *= 2u;
        nd = (uint8_t *)realloc(b->d, nc);
        if (!nd) return;
        b->d = nd;
        b->cap = nc;
    }
    if (n) memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void dt_u32(DtBuf *b, uint32_t v) {
    uint8_t t[4];
    t[0] = (uint8_t)(v >> 24); t[1] = (uint8_t)(v >> 16);
    t[2] = (uint8_t)(v >> 8);  t[3] = (uint8_t)v;
    dt_put(b, t, 4);
}

static void dt_zero(DtBuf *b, size_t n) {
    static const uint8_t z[64] = {0};
    while (n > 0) {
        size_t k = n < sizeof(z) ? n : sizeof(z);
        dt_put(b, z, k);
        n -= k;
    }
}

static size_t dt_open(DtBuf *b, const char *type) {
    size_t at = b->n;
    dt_u32(b, 0);
    dt_put(b, type, 4);
    return at;
}

static void dt_patch(DtBuf *b, size_t at, uint32_t v) {
    b->d[at]     = (uint8_t)(v >> 24);
    b->d[at + 1] = (uint8_t)(v >> 16);
    b->d[at + 2] = (uint8_t)(v >> 8);
    b->d[at + 3] = (uint8_t)v;
}

static void dt_close(DtBuf *b, size_t at) {
    dt_patch(b, at, (uint32_t)(b->n - at));
}

static void dt_free(DtBuf *b) {
    free(b->d);
    memset(b, 0, sizeof(*b));
}

/* اندازه و محتوای نمونه‌ها؛ هر دو طرف آزمون از همین تابع استفاده می‌کنند */
static uint32_t dt_sample_size(uint32_t seq, uint32_t i) {
    return 100u + seq * 10u + i * 7u;
}

static uint8_t dt_sample_byte(uint32_t track, uint32_t seq, uint32_t i, uint32_t j) {
    return (uint8_t)(track * 101u + seq * 13u + i * 17u + j * 5u);
}

/* init: ftyp + moov با mvhd، trak (tkhd، mdia با mdhd و hdlr و minf و stsd)، mvex/trex */
static void dt_init(DtBuf *b, uint32_t track, int video, uint32_t timescale,
                    uint32_t trex_dur, uint32_t trex_flags, uint32_t mvhd_ts, int with_edts) {
    size_t x, y, z, w, u;
    x = dt_open(b, "ftyp");
    dt_put(b, "isom", 4); dt_u32(b, 512); dt_put(b, "isom", 4); dt_put(b, "iso2", 4);
    dt_close(b, x);

    x = dt_open(b, "moov");
    y = dt_open(b, "mvhd");
    dt_u32(b, 0); dt_u32(b, 0); dt_u32(b, 0); dt_u32(b, mvhd_ts); dt_u32(b, 0);
    dt_zero(b, 80);
    dt_close(b, y);

    y = dt_open(b, "trak");
    z = dt_open(b, "tkhd");
    dt_u32(b, 3); dt_u32(b, 0); dt_u32(b, 0); dt_u32(b, track); dt_u32(b, 0); dt_u32(b, 0);
    dt_zero(b, 52);
    dt_u32(b, video ? (160u << 16) : 0u);
    dt_u32(b, video ? (120u << 16) : 0u);
    dt_close(b, z);
    if (with_edts) {
        /* edts/elst: مدت ۱۰۰۰ در مقیاس mvhd (۵۰۰) = ۲۰۰۰ در مقیاس ۱۰۰۰ خروجی */
        size_t ed = dt_open(b, "edts");
        size_t el = dt_open(b, "elst");
        dt_u32(b, 0); dt_u32(b, 1); dt_u32(b, 1000u); dt_u32(b, 1024u); dt_u32(b, 0x00010000u);
        dt_close(b, el);
        dt_close(b, ed);
    }
    z = dt_open(b, "mdia");
    w = dt_open(b, "mdhd");
    dt_u32(b, 0); dt_u32(b, 0); dt_u32(b, 0); dt_u32(b, timescale); dt_u32(b, 0); dt_u32(b, 0x55C40000u);
    dt_close(b, w);
    w = dt_open(b, "hdlr");
    dt_u32(b, 0); dt_u32(b, 0);
    dt_put(b, video ? "vide" : "soun", 4);
    dt_zero(b, 12);
    dt_put(b, "\0", 1);
    dt_close(b, w);

    w = dt_open(b, "minf");
    u = dt_open(b, video ? "vmhd" : "smhd");
    dt_u32(b, video ? 1u : 0u);
    dt_zero(b, video ? 8u : 4u);
    dt_close(b, u);
    u = dt_open(b, "dinf");
    {
        size_t r = dt_open(b, "dref");
        size_t e;
        dt_u32(b, 0); dt_u32(b, 1);
        e = dt_open(b, "url ");
        dt_u32(b, 1);
        dt_close(b, e);
        dt_close(b, r);
    }
    dt_close(b, u);
    u = dt_open(b, "stbl");
    {
        size_t sd = dt_open(b, "stsd");
        size_t e;
        dt_u32(b, 0); dt_u32(b, 1);
        e = dt_open(b, video ? "avc1" : "mp4a");
        dt_zero(b, 8);
        dt_close(b, e);
        dt_close(b, sd);
    }
    dt_close(b, u);
    dt_close(b, w);
    dt_close(b, z);
    dt_close(b, y);

    y = dt_open(b, "mvex");
    z = dt_open(b, "trex");
    dt_u32(b, 0); dt_u32(b, track); dt_u32(b, 1); dt_u32(b, trex_dur); dt_u32(b, 0); dt_u32(b, trex_flags);
    dt_close(b, z);
    dt_close(b, y);
    dt_close(b, x);
}

/* قطعه: moof (mfhd، traf با tfhd و trun) و mdat؛ data_offset نسبت به شروع moof */
static void dt_segment(DtBuf *b, uint32_t track, int video, uint32_t seq,
                       uint32_t count, uint32_t dur_each) {
    size_t moof_at, mfhd, traf, tfhd, trun, doff_pos, mdat;
    uint32_t i, j, moof_size, flags;
    moof_at = dt_open(b, "moof");
    mfhd = dt_open(b, "mfhd");
    dt_u32(b, 0); dt_u32(b, seq);
    dt_close(b, mfhd);
    traf = dt_open(b, "traf");
    tfhd = dt_open(b, "tfhd");
    dt_u32(b, 0x00020000u);
    dt_u32(b, track);
    dt_close(b, tfhd);
    trun = dt_open(b, "trun");
    flags = video ? 0x00000305u : 0x00000301u;
    dt_u32(b, flags);
    dt_u32(b, count);
    doff_pos = b->n;
    dt_u32(b, 0);
    if (video) dt_u32(b, 0x02000000u);
    for (i = 0; i < count; i++) {
        dt_u32(b, dur_each);
        dt_u32(b, dt_sample_size(seq, i));
    }
    dt_close(b, trun);
    dt_close(b, traf);
    dt_close(b, moof_at);
    moof_size = (uint32_t)(b->n - moof_at);
    dt_patch(b, doff_pos, moof_size + 8u);
    mdat = dt_open(b, "mdat");
    for (i = 0; i < count; i++) {
        for (j = 0; j < dt_sample_size(seq, i); j++) {
            uint8_t c = dt_sample_byte(track, seq, i, j);
            dt_put(b, &c, 1);
        }
    }
    dt_close(b, mdat);
}

/* محتوای مورد انتظار خروجی: نمونه‌های video سه قطعه، سپس audio سه قطعه */
static void dt_expected(DtBuf *out) {
    uint32_t seq, i, j;
    for (seq = 1; seq <= 3; seq++) {
        for (i = 0; i < 4; i++) {
            for (j = 0; j < dt_sample_size(seq, i); j++) {
                uint8_t c = dt_sample_byte(1, seq, i, j);
                dt_put(out, &c, 1);
            }
        }
    }
    for (seq = 1; seq <= 3; seq++) {
        for (i = 0; i < 3; i++) {
            for (j = 0; j < dt_sample_size(seq, i); j++) {
                uint8_t c = dt_sample_byte(2, seq, i, j);
                dt_put(out, &c, 1);
            }
        }
    }
}

typedef struct { uint32_t n; uint32_t size[8]; uint32_t dur[8]; BOOL sync[8]; } DtSamples;

static BOOL dt_collect(const Mp4FragSample *s, void *ctx) {
    DtSamples *d = (DtSamples *)ctx;
    if (d->n >= 8) return FALSE;
    d->size[d->n] = s->size;
    d->dur[d->n] = s->duration;
    d->sync[d->n] = s->sync;
    d->n++;
    return TRUE;
}

static BOOL dt_has(const char *s, const char *sub) {
    return s && strstr(s, sub) != NULL;
}

static uint8_t *dt_read_file(const char *path, size_t *len) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    uint8_t *b;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h);
        return NULL;
    }
    b = (uint8_t *)malloc((size_t)sz.QuadPart);
    if (!b || !ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        free(b);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *len = (size_t)sz.QuadPart;
    return b;
}

/* سرآیند mdat در سطح بالا: آفست داده و طول آن */
static BOOL dt_find_mdat(const uint8_t *f, size_t n, size_t *pay, size_t *plen) {
    size_t off = 0;
    while (off + 8 <= n) {
        uint32_t sz = ((uint32_t)f[off] << 24) | ((uint32_t)f[off + 1] << 16) |
                      ((uint32_t)f[off + 2] << 8) | (uint32_t)f[off + 3];
        if (sz < 8 || off + sz > n) return FALSE;
        if (memcmp(f + off + 4, "mdat", 4) == 0) {
            *pay = off + 8;
            *plen = sz - 8;
            return TRUE;
        }
        off += sz;
    }
    return FALSE;
}

static BOOL dt_contains(const uint8_t *f, size_t n, const char *needle) {
    size_t nl = strlen(needle), i;
    for (i = 0; i + nl <= n; i++) {
        if (memcmp(f + i, needle, nl) == 0) return TRUE;
    }
    return FALSE;
}

/* خروجی: ftyp، mdat با محتوای مورد انتظار، هر دو track در moov */
static BOOL dt_check_output(const char *path, const DtBuf *expected) {
    size_t flen = 0, pay = 0, plen = 0;
    uint8_t *fb = dt_read_file(path, &flen);
    BOOL ok = FALSE;
    if (fb && flen > 16 && memcmp(fb + 4, "ftyp", 4) == 0 && dt_find_mdat(fb, flen, &pay, &plen)) {
        ok = plen == expected->n && memcmp(fb + pay, expected->d, expected->n) == 0 &&
             dt_contains(fb, flen, "vide") && dt_contains(fb, flen, "soun");
    }
    free(fb);
    return ok;
}

/* elst خروجی باید مدت ۲۰۰۰ در مقیاس ۱۰۰۰ و media_time بدون تغییر داشته باشد */
static BOOL dt_elst_ok(const char *path) {
    size_t n = 0, i;
    uint8_t *f = dt_read_file(path, &n);
    BOOL ok = FALSE;
    if (f) {
        for (i = 4; i + 24 <= n; i++) {
            if (memcmp(f + i, "elst", 4) == 0) {
                const uint8_t *p = f + i + 4;
                uint32_t cnt = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
                uint32_t seg = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
                uint32_t med = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16) | ((uint32_t)p[14] << 8) | p[15];
                ok = cnt == 1u && seg == 2000u && med == 1024u;
                break;
            }
        }
    }
    free(f);
    return ok;
}

static const char DT_MPD_TEXT[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" mediaPresentationDuration=\"PT0.48S\">\n"
    "<Period id=\"1\" start=\"PT0S\">\n"
    "<AdaptationSet mimeType=\"video/mp4\" contentType=\"video\" width=\"160\" height=\"120\">\n"
    "<Representation id=\"v1\" bandwidth=\"500000\" codecs=\"avc1.42c01e\">\n"
    "<SegmentTemplate timescale=\"12800\" duration=\"2048\" startNumber=\"1\" "
    "initialization=\"v-init.mp4\" media=\"v-$Number$.m4s\"/>\n"
    "</Representation>\n"
    "</AdaptationSet>\n"
    "<AdaptationSet mimeType=\"audio/mp4\" contentType=\"audio\">\n"
    "<Representation id=\"a1\" bandwidth=\"64000\" codecs=\"mp4a.40.2\">\n"
    "<SegmentList timescale=\"44100\" duration=\"3072\">\n"
    "<Initialization sourceURL=\"a-init.mp4\"/>\n"
    "<SegmentURL media=\"a-1.m4s\"/>\n"
    "<SegmentURL media=\"a-2.m4s\"/>\n"
    "<SegmentURL media=\"a-3.m4s\"/>\n"
    "</SegmentList>\n"
    "</Representation>\n"
    "</AdaptationSet>\n"
    "</Period>\n"
    "</MPD>\n";

static const char DT_MPD_FMT[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT0.5S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
    "<Representation id=\"x\" bandwidth=\"1000\"><SegmentTemplate timescale=\"1000\" duration=\"500\" "
    "media=\"seg-$Number%05d$.m4s\"/></Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_LIVE[] =
    "<MPD type=\"dynamic\" mediaPresentationDuration=\"PT1S\"><Period>"
    "<AdaptationSet mimeType=\"video/mp4\"><Representation id=\"v\" bandwidth=\"1\">"
    "<SegmentTemplate timescale=\"1\" duration=\"1\" media=\"x-$Number$.m4s\"/>"
    "</Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_MULTI[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period/><Period/></MPD>";

static const char DT_MPD_DRM[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
    "<ContentProtection schemeIdUri=\"urn:uuid:x\"/>"
    "<Representation id=\"v\" bandwidth=\"1\"><SegmentTemplate timescale=\"1\" duration=\"1\" "
    "media=\"x-$Number$.m4s\"/></Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_TIMELINE[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
    "<Representation id=\"v\" bandwidth=\"1\"><SegmentTemplate timescale=\"1\" media=\"x-$Number$.m4s\">"
    "<SegmentTimeline><S t=\"0\" d=\"1\" r=\"0\"/></SegmentTimeline></SegmentTemplate>"
    "</Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_SEGBASE[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
    "<Representation id=\"v\" bandwidth=\"1\"><SegmentBase indexRange=\"0-100\"/>"
    "</Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_WEBM[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet mimeType=\"video/webm\">"
    "<Representation id=\"v\" bandwidth=\"1\"><SegmentTemplate timescale=\"1\" duration=\"1\" "
    "media=\"x-$Number$.webm\"/></Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_EVIL[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
    "<Representation id=\"v\" bandwidth=\"1\"><SegmentTemplate timescale=\"1\" duration=\"1\" "
    "media=\"x-$Number%n$.m4s\"/></Representation></AdaptationSet></Period></MPD>";

static const char DT_MPD_TIME[] =
    "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
    "<Representation id=\"v\" bandwidth=\"1\"><SegmentTemplate timescale=\"1\" duration=\"1\" "
    "media=\"x-$Time$.m4s\"/></Representation></AdaptationSet></Period></MPD>";

static const char DT_HTML_PAGE[] = "<html><body>not a manifest</body></html>";

static BOOL dt_parse(DASH_Manifest *mf, const char *base, const char *text) {
    dash_manifest_free(mf);
    return dash_parse_manifest(base, text, strlen(text), mf);
}

typedef struct { int stop_at; uint32_t seen; } DtStop;

static int dt_should_stop(void *ctx) {
    DtStop *s = (DtStop *)ctx;
    return (s->stop_at > 0 && s->seen >= (uint32_t)s->stop_at) ? 1 : 0;
}

static void dt_progress(uint64_t bytes, uint32_t done, uint32_t total, void *ctx) {
    DtStop *s = (DtStop *)ctx;
    (void)bytes;
    (void)total;
    s->seen = done;
}

static void dt_serve(const DtBuf *vi, const DtBuf *ai, const DtBuf *vseg, const DtBuf *aseg,
                     const DtBuf *mpd) {
    char path[64];
    uint32_t k;
    hls_test_server_set("/manifest.mpd", mpd->d, (uint32_t)mpd->n);
    hls_test_server_set("/v-init.mp4", vi->d, (uint32_t)vi->n);
    hls_test_server_set("/a-init.mp4", ai->d, (uint32_t)ai->n);
    for (k = 1; k <= 3; k++) {
        snprintf(path, sizeof(path), "/v-%u.m4s", (unsigned)k);
        hls_test_server_set(path, vseg[k - 1].d, (uint32_t)vseg[k - 1].n);
        snprintf(path, sizeof(path), "/a-%u.m4s", (unsigned)k);
        hls_test_server_set(path, aseg[k - 1].d, (uint32_t)aseg[k - 1].n);
    }
}

void test_dash_media(void) {
    DtBuf vi = {0}, ai = {0}, mpd_b = {0}, expected = {0};
    DtBuf vseg[3] = {{0}, {0}, {0}};
    DtBuf aseg[3] = {{0}, {0}, {0}};
    DASH_Manifest *mf;
    char url[DASH_MAX_URL];
    uint32_t k;

    SECTION("DASH: fragmented MP4 parsing");
    dt_init(&vi, 1, 1, 12800u, 512u, 0x01010000u, 1000u, 0);
    dt_init(&ai, 2, 0, 44100u, 1024u, 0u, 500u, 1);
    for (k = 1; k <= 3; k++) {
        dt_segment(&vseg[k - 1], 1, 1, k, 4u, 512u);
        dt_segment(&aseg[k - 1], 2, 0, k, 3u, 1024u);
    }
    {
        Mp4Init vin, ain;
        char err[128];
        DtSamples sm;
        BOOL okv = mp4_parse_init(vi.d, vi.n, &vin, err, sizeof(err));
        BOOL oka = mp4_parse_init(ai.d, ai.n, &ain, err, sizeof(err));
        TEST("video init parses", okv && vin.track_count == 1);
        TEST("video timescale is 12800", okv && vin.track[0].timescale == 12800u);
        TEST("video handler is vide", okv && memcmp(vin.track[0].handler, "vide", 4) == 0);
        TEST("audio init parses", oka && ain.track_count == 1 && ain.track[0].timescale == 44100u);
        {
            Mp4Init vbad;
            TEST("truncated init is rejected", !mp4_parse_init(vi.d, vi.n / 2, &vbad, err, sizeof(err)));
        }

        memset(&sm, 0, sizeof(sm));
        TEST("video fragment walks", mp4_walk_fragments(vseg[0].d, vseg[0].n, &vin, dt_collect, &sm, err, sizeof(err)));
        TEST("video fragment has 4 samples", sm.n == 4u);
        TEST("sample sizes match the fixture", sm.n == 4u && sm.size[0] == dt_sample_size(1, 0) && sm.size[3] == dt_sample_size(1, 3));
        TEST("durations come from trun", sm.n == 4u && sm.dur[0] == 512u && sm.dur[3] == 512u);
        TEST("first sample is sync, the rest are not", sm.n == 4u && sm.sync[0] && !sm.sync[1] && !sm.sync[2]);

        memset(&sm, 0, sizeof(sm));
        TEST("audio fragment walks", mp4_walk_fragments(aseg[1].d, aseg[1].n, &ain, dt_collect, &sm, err, sizeof(err)));
        TEST("audio fragment has 3 samples, all sync", sm.n == 3u && sm.sync[0] && sm.sync[1] && sm.sync[2]);
    }

    SECTION("DASH: manifest rules and URL expansion");
    mf = (DASH_Manifest *)calloc(1, sizeof(DASH_Manifest));
    if (!mf) {
        TEST("manifest allocation", FALSE);
        dt_free(&vi); dt_free(&ai);
        return;
    }
    TEST("valid manifest parses", dt_parse(mf, DT_BASE "/manifest.mpd", DT_MPD_TEXT));
    TEST("valid manifest is supported", mf->unsupported[0] == '\0');
    TEST("one video and one audio representation", mf->video_count == 1 && mf->audio_count == 1);
    TEST("video template gives 3 segments", dash_rep_segment_count(&mf->video[0]) == 3u);
    TEST("video init URL is resolved", strcmp(mf->video[0].init_url, DT_BASE "/v-init.mp4") == 0);
    TEST("video segment 2 URL is expanded", dash_rep_segment_url(&mf->video[0], 1, url, sizeof(url)) &&
                                            strcmp(url, DT_BASE "/v-2.m4s") == 0);
    TEST("index past the last segment is refused", !dash_rep_segment_url(&mf->video[0], 3, url, sizeof(url)));
    TEST("audio SegmentList gives 3 segments", dash_rep_segment_count(&mf->audio[0]) == 3u);
    TEST("audio SegmentList URL is absolute", dash_rep_segment_url(&mf->audio[0], 0, url, sizeof(url)) &&
                                              strcmp(url, DT_BASE "/a-1.m4s") == 0);
    TEST("audio init URL is resolved", strcmp(mf->audio[0].init_url, DT_BASE "/a-init.mp4") == 0);
    TEST("best representations are chosen", mf->best_video == 0 && mf->best_audio == 0);
    TEST("$Number%05d$ expands with zero padding",
         dt_parse(mf, "http://h.example/dir/m.mpd", DT_MPD_FMT) &&
         dash_rep_segment_url(&mf->video[0], 0, url, sizeof(url)) &&
         strcmp(url, "http://h.example/dir/seg-00001.m4s") == 0);
    TEST("unsafe format $Number%n$ is refused",
         dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_EVIL) && mf->video_count == 0 &&
         dt_has(mf->unsupported, "unsupported variable"));
    TEST("two periods are refused", dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_MULTI) &&
                                    dt_has(mf->unsupported, "more than one Period"));
    TEST("DRM is refused", dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_DRM) && dt_has(mf->unsupported, "DRM"));
    TEST("live manifest is flagged and refused",
         dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_LIVE) && mf->is_live && dt_has(mf->unsupported, "live"));
    TEST("SegmentTimeline is refused", dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_TIMELINE) &&
                                       dt_has(mf->unsupported, "SegmentTimeline"));
    TEST("SegmentBase is refused", dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_SEGBASE) &&
                                   dt_has(mf->unsupported, "SegmentBase"));
    TEST("WebM is refused", dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_WEBM) && dt_has(mf->unsupported, "WebM"));
    TEST("$Time$ templates are refused", dt_parse(mf, DT_BASE "/m.mpd", DT_MPD_TIME) &&
                                         dt_has(mf->unsupported, "$Time$"));
    TEST("HTML page is not a manifest", !dt_parse(mf, DT_BASE "/m.mpd", DT_HTML_PAGE));
    TEST("DASH downloader accepts .mpd URLs only",
         dash_is_mpd_url("http://x.example/a.mpd") && !dash_is_mpd_url("http://x.example/a.m3u8"));
    dash_manifest_free(mf);
    free(mf);

    SECTION("DASH: full download, resume, stop, and refusal through the downloader");
    if (!hls_test_server_start(DT_PORT)) {
        TEST("test server starts", FALSE);
        dt_free(&vi); dt_free(&ai);
        return;
    }
    dt_put(&mpd_b, DT_MPD_TEXT, strlen(DT_MPD_TEXT));
    dt_expected(&expected);
    dt_serve(&vi, &ai, vseg, aseg, &mpd_b);
    hls_test_server_set("/live.mpd", (const uint8_t *)DT_MPD_LIVE, (uint32_t)strlen(DT_MPD_LIVE));

    {
        DashRun run;
        DtStop none = {0, 0};
        DashStatus st;
        DeleteFileA("dash_t1.tels");
        DeleteFileA("dash_t1_out.mp4");
        hls_test_server_reset_hits();
        memset(&run, 0, sizeof(run));
        run.url = DT_BASE "/manifest.mpd";
        run.temp_path = "dash_t1.tels";
        run.out_path = "dash_t1_out.mp4";
        run.should_stop = dt_should_stop;
        run.stop_ctx = &none;
        run.on_progress = dt_progress;
        run.progress_ctx = &none;
        st = dash_download_run(&run);
        TEST("full download completes", st == DASH_OK);
        TEST("output file name ends with .mp4", run.final_path[0] != '\0' && dt_has(run.final_path, ".mp4"));
        TEST("output holds the expected media bytes in order", dt_check_output(run.final_path, &expected));
        TEST("edit list is copied and rescaled to the 1000 movie timescale", dt_elst_ok(run.final_path));
        TEST("temporary container is removed after success", GetFileAttributesA("dash_t1.tels") == INVALID_FILE_ATTRIBUTES);
        TEST("each video segment is fetched once", hls_test_server_hits("/v-1.m4s") == 1);
        DeleteFileA(run.final_path);
    }
    {
        DashRun run;
        DtStop none = {0, 0};
        DashStatus st;
        DeleteFileA("dash_t2.tels");
        DeleteFileA("dash_t2_out.mp4");
        hls_test_server_reset_hits();
        hls_test_server_fail("/v-2.m4s", 404, 1000);
        memset(&run, 0, sizeof(run));
        run.url = DT_BASE "/manifest.mpd";
        run.temp_path = "dash_t2.tels";
        run.out_path = "dash_t2_out.mp4";
        run.should_stop = dt_should_stop;
        run.stop_ctx = &none;
        run.on_progress = dt_progress;
        run.progress_ctx = &none;
        st = dash_download_run(&run);
        TEST("missing segment fails the run as not found", st == DASH_ERR_NOT_FOUND);
        TEST("the failure names the segment", dt_has(run.error, "video segment #2"));
        TEST("the container is kept for resume", GetFileAttributesA("dash_t2.tels") != INVALID_FILE_ATTRIBUTES);
        hls_test_server_clear_fail("/v-2.m4s");
        st = dash_download_run(&run);
        TEST("the next run completes the download", st == DASH_OK);
        TEST("finished segments are not fetched again", hls_test_server_hits("/v-1.m4s") == 1);
        TEST("resumed output holds the expected media bytes", dt_check_output(run.final_path, &expected));
        DeleteFileA(run.final_path);
    }
    {
        DashRun run;
        DtStop stop = {3, 0};
        DtStop none = {0, 0};
        DashStatus st;
        DeleteFileA("dash_t3.tels");
        DeleteFileA("dash_t3_out.mp4");
        hls_test_server_reset_hits();
        memset(&run, 0, sizeof(run));
        run.url = DT_BASE "/manifest.mpd";
        run.temp_path = "dash_t3.tels";
        run.out_path = "dash_t3_out.mp4";
        run.should_stop = dt_should_stop;
        run.stop_ctx = &stop;
        run.on_progress = dt_progress;
        run.progress_ctx = &stop;
        st = dash_download_run(&run);
        TEST("stop request ends the run as stopped", st == DASH_ERR_STOPPED);
        TEST("stopped run keeps the container", GetFileAttributesA("dash_t3.tels") != INVALID_FILE_ATTRIBUTES);
        TEST("stopped run leaves no output file", GetFileAttributesA("dash_t3_out.mp4") == INVALID_FILE_ATTRIBUTES);
        run.should_stop = dt_should_stop;
        run.stop_ctx = &none;
        run.progress_ctx = &none;
        st = dash_download_run(&run);
        TEST("resumed run completes", st == DASH_OK);
        TEST("resumed run fetched each segment once", hls_test_server_hits("/v-1.m4s") == 1);
        TEST("stopped-then-resumed output is byte-identical", dt_check_output(run.final_path, &expected));
        DeleteFileA(run.final_path);
    }
    {
        DashRun run;
        DashStatus st;
        memset(&run, 0, sizeof(run));
        run.url = DT_BASE "/live.mpd";
        run.temp_path = "dash_t4.tels";
        run.out_path = "dash_t4_out.mp4";
        st = dash_download_run(&run);
        TEST("live manifest is refused by the downloader", st == DASH_ERR_UNSUPPORTED && dt_has(run.error, "live"));
        TEST("refused manifest creates no container", GetFileAttributesA("dash_t4.tels") == INVALID_FILE_ATTRIBUTES);
    }
    hls_test_server_stop();
    DeleteFileA("dash_t1.tels");
    DeleteFileA("dash_t2.tels");
    DeleteFileA("dash_t3.tels");
    DeleteFileA("dash_t4.tels");
    dt_free(&vi);
    dt_free(&ai);
    dt_free(&mpd_b);
    dt_free(&expected);
    for (k = 0; k < 3; k++) {
        dt_free(&vseg[k]);
        dt_free(&aseg[k]);
    }
}

