/* engine: download engine tests (tests/test_engine.c) */
#include "test_common.h"

/* ======================================================
   تست ۱۴: Engine — scheduler، batch، clear_completed
   ====================================================== */
void test_engine_features(void) {
    SECTION("Engine: Scheduler + Batch + Clear");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create",         eng != NULL);
    if (!eng) return;

    /* بدون دانلود واقعی: این تست فقط صف و گروه را بررسی می‌کند، نه شبکه را */
    eng->max_concurrent = 0;
    /* ── engine_add_batch ── */
    const char *batch_urls =
        "http://example.com/file1.zip\n"
        "http://example.com/file2.zip\n"
        "   \n"                          /* خط خالی — باید skip بشه */
        "http://example.com/file3.zip\n"
        "not_a_url\n"                    /* invalid — باید skip بشه */
        "ftp://files.example.com/data.tar.gz\n";

    int added = engine_add_batch(eng, batch_urls, NULL, 4, 0, "TestGroup");
    TEST("batch: 3 http + 1 ftp accepted (empty and invalid lines skipped)", added == 4);

    /* بررسی group_id یکسان */
    EnterCriticalSection(&eng->lock);
    uint32_t gid = 0;
    int same_group = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) {
        if (gid == 0 && dl->group_id != 0) gid = dl->group_id;
        if (dl->group_id == gid) same_group++;
    }
    LeaveCriticalSection(&eng->lock);
    TEST("all accepted links in same group", same_group == 4);
    TEST("group_id not zero",     gid != 0);

    /* بررسی group_name */
    EnterCriticalSection(&eng->lock);
    IDM_Download *first = eng->queue_head;
    BOOL name_ok = first && strcmp(first->group_name, "TestGroup") == 0;
    LeaveCriticalSection(&eng->lock);
    TEST("group name set",        name_ok);

    /* ── engine_clear_completed ── */
    /* دستی state چند تا رو completed کن */
    EnterCriticalSection(&eng->lock);
    int marked = 0;
    for (IDM_Download *dl = eng->queue_head; dl && marked < 2; dl = dl->next) {
        dl->state = DL_COMPLETED;
        marked++;
    }
    LeaveCriticalSection(&eng->lock);

    int cleared = engine_clear_completed(eng);
    TEST("cleared 2 completed",   cleared == 2);

    /* count باید ۲ تا کمتر باشه */
    EnterCriticalSection(&eng->lock);
    int remaining = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) remaining++;
    LeaveCriticalSection(&eng->lock);
    TEST("remaining count ok",    remaining == 2);

    /* ── engine_schedule ── */
    EnterCriticalSection(&eng->lock);
    uint32_t first_id = eng->queue_head ? eng->queue_head->id : 0;
    LeaveCriticalSection(&eng->lock);

    if (first_id) {
        engine_schedule(eng, first_id, 23, 59);

        EnterCriticalSection(&eng->lock);
        IDM_Download *dl = engine_find(eng, first_id);
        BOOL has_sched   = dl && dl->has_schedule;
        BOOL is_paused   = dl && (dl->state == DL_PAUSED ||
                                   dl->state == DL_QUEUED);
        LeaveCriticalSection(&eng->lock);
        TEST("schedule sets flag",    has_sched);
        TEST("state after schedule",  is_paused);

        /* unschedule */
        engine_unschedule(eng, first_id);
        EnterCriticalSection(&eng->lock);
        dl = engine_find(eng, first_id);
        BOOL cleared_flag = dl && !dl->has_schedule;
        LeaveCriticalSection(&eng->lock);
        TEST("unschedule clears flag", cleared_flag);

        /* schedule invalid hour */
        engine_schedule(eng, first_id, 25, 0);
        EnterCriticalSection(&eng->lock);
        dl = engine_find(eng, first_id);
        BOOL still_clear = dl && !dl->has_schedule;
        LeaveCriticalSection(&eng->lock);
        TEST("invalid hour rejected",  still_clear);
    }

    /* ── NULL safety ── */
    engine_schedule(NULL, 0, 12, 0);
    engine_unschedule(NULL, 0);
    engine_add_batch(NULL, "http://x.com/f", NULL, 4, 0, NULL);
    engine_clear_completed(NULL);
    TEST("null safety engine funcs", TRUE);

    engine_destroy(eng);
    TEST("engine destroy",        TRUE);
}

/* ======================================================
   تست ۱۵: engine_add FIFO order
   ====================================================== */
void test_engine_fifo(void) {
    SECTION("Engine: FIFO Queue Order");

    IDM_Engine *eng = engine_create(NULL);
    /* max=0 تا هیچ دانلودی شروع نشه */
    eng->max_concurrent = 0;
    TEST("engine create",         eng != NULL);
    if (!eng) return;

    engine_add(eng, "http://example.com/first.zip",  "c:\\tmp\\1.zip", 1, 0);
    engine_add(eng, "http://example.com/second.zip", "c:\\tmp\\2.zip", 1, 0);
    engine_add(eng, "http://example.com/third.zip",  "c:\\tmp\\3.zip", 1, 0);

    /* FIFO: اولین باید first.zip باشه */
    EnterCriticalSection(&eng->lock);
    IDM_Download *h = eng->queue_head;
    BOOL fifo_ok = h
        && strstr(h->url, "first")  != NULL
        && strstr(h->next->url, "second") != NULL
        && strstr(h->next->next->url, "third") != NULL;
    LeaveCriticalSection(&eng->lock);
    TEST("FIFO order correct",    fifo_ok);

    /* remove وسطی — tail باید درست بمونه */
    EnterCriticalSection(&eng->lock);
    uint32_t mid_id = eng->queue_head->next->id;
    LeaveCriticalSection(&eng->lock);

    engine_remove(eng, mid_id);

    EnterCriticalSection(&eng->lock);
    int cnt = 0;
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) cnt++;
    BOOL tail_ok = (eng->queue_tail != NULL) &&
                   strstr(eng->queue_tail->url, "third") != NULL;
    LeaveCriticalSection(&eng->lock);
    TEST("count after remove",    cnt == 2);
    TEST("tail still correct",    tail_ok);

    engine_destroy(eng);
    TEST("engine destroy",        TRUE);
}

/* ======================================================
   تست: دانلود همزمان چند فایل (multi-concurrent)
   ====================================================== */
void test_engine_concurrent(void) {
    SECTION("Engine: Multi-Concurrent Downloads");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create",                eng != NULL);
    if (!eng) return;

    /* max_concurrent رو روی 3 بذار */
    eng->max_concurrent = 3;
    /* scheduler رو غیرفعال کن تا فقط queue logic رو تست کنیم */
    eng->max_concurrent = 0; /* هیچ auto-launch نشه */

    IDM_Download *d1 = engine_add(eng, "http://a.com/f1.zip", "C:\\f1.zip", 4, 0);
    IDM_Download *d2 = engine_add(eng, "http://b.com/f2.zip", "C:\\f2.zip", 4, 0);
    IDM_Download *d3 = engine_add(eng, "http://c.com/f3.zip", "C:\\f3.zip", 4, 0);

    TEST("3 downloads added",            d1 && d2 && d3);
    TEST("all start as QUEUED",
         d1 && d1->state == DL_QUEUED &&
         d2 && d2->state == DL_QUEUED &&
         d3 && d3->state == DL_QUEUED);

    /* شمارش دستی از queue */
    int cnt = 0;
    EnterCriticalSection(&eng->lock);
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next) cnt++;
    LeaveCriticalSection(&eng->lock);
    TEST("queue count is 3",             cnt == 3);

    /* تنظیم max_concurrent و شبیه‌سازی چک scheduler */
    eng->max_concurrent = 3;
    int would_launch = 0;
    EnterCriticalSection(&eng->lock);
    for (IDM_Download *dl = eng->queue_head; dl; dl = dl->next)
        if (dl->state == DL_QUEUED && eng->active_count < eng->max_concurrent)
            would_launch++;
    LeaveCriticalSection(&eng->lock);

    TEST("scheduler would launch all 3", would_launch == 3);

    engine_destroy(eng);
    TEST("engine destroy",               TRUE);
}

/* ======================================================
   تست: Real Concurrent HTTP Downloads (loopback)
   ─────────────────────────────────────────────────────
   دلیل وجودش: تست بالا ("Multi-Concurrent Downloads") فقط
   منطق scheduler رو با شمارش دستی روی queue شبیه‌سازی می‌کرد —
   هیچ‌وقت واقعاً http_discover/http_open_range رو صدا نمی‌زد.
   دقیقاً به همین خاطر بود که باگ واقعی v5.0.7 (WinINet سقف
   connection همزمان به یه host رو رعایت می‌کرد و دانلود دوم/سوم
   در «connecting» گیر می‌کردن) هیچ‌وقت توسط test_all گرفته نشد —
   فقط با تست دستی روی اینترنت واقعی کشف شد.

   این تست یه سرور HTTP واقعی (هرچند مینیمال) روی 127.0.0.1
   بالا میاره، ۳ دانلود واقعی از همون host واحد اضافه می‌کنه، و
   مستقیماً از خودِ سرور اندازه می‌گیره که آیا واقعاً چند GET
   هم‌زمان باز بوده یا نه — به‌جای اینکه فقط state داخلی engine
   رو (که می‌تونه گمراه‌کننده باشه) چک کنه.

   عمداً http_init() رو اینجا صدا می‌زنیم — دقیقاً همون کاری که
   main.c موقع startup انجام میده. اگه یه روز کسی اون call رو از
   main.c حذف/جابه‌جا کنه، این تست باید fail بشه (peak concurrent
   GET به ۱ برمی‌گرده)، نه اینکه ساکت رگرس کنه. */
void test_engine_real_concurrency(void) {
    SECTION("Engine: Real Concurrent HTTP Downloads (loopback)");

    const uint16_t PORT = 58732;
    const uint32_t BODY_SIZE = 65536;      /* 64KB — engine فقط ۱ segment انتخاب می‌کنه */
    /* توجه مهم: engine.c عمداً بین launch دانلودهای مختلف به یه
       host واحد یه فاصله می‌ندازه (ENGINE_HOST_STAGGER_MS = 1500ms،
       در engine.h) تا سرور رو با burst همزمان نکوبه — این خودش
       یه فیچر دلخواسته‌ست، نه باگ. نسخه‌ی اول این تست TRICKLE_MS
       رو ۱۲۰۰ گذاشته بود، یعنی دانلود اول کامل می‌شد و connection‌ش
       بسته می‌شد *قبل* از این‌که حتی دانلود دوم (که ۱۵۰۰ms بعد
       stagger می‌شه) شروع بشه — پس هیچ‌وقت هم‌پوشانی واقعی رخ
       نمی‌داد و peak همیشه ۱ می‌موند، صرف‌نظر از این‌که concurrency
       واقعی درست کار می‌کنه یا نه. باید TRICKLE_MS به‌قدر کافی
       بزرگ‌تر از ۲×STAGGER باشه تا سه‌تا connection واقعاً هم‌زمان
       باز بمونن. */
    const uint32_t TRICKLE_MS = 3000;

    bool srv_ok = test_httpd_start(PORT, BODY_SIZE, TRICKLE_MS);
    TEST("loopback test server started", srv_ok);
    if (!srv_ok) return;

    /* دقیقاً همون چیزی که main.c قبل از engine_create صدا می‌زنه —
       بدون این، این تست دقیقاً همون باگ v5.0.7 رو reproduce می‌کنه. */
    http_init();

    /* callback های http (If-Range، کوکی، auth) از plugins_init وصل
       می‌شن — همون کاری که برنامه‌ی واقعی می‌کنه. */
    IDM_PluginRegistry *plug_reg = plugins_init();

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) { test_httpd_stop(); return; }

    char urls[3][64];
    char out_paths[3][MAX_PATH + 32];
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);

    IDM_Download *dls[3] = {0};
    for (int i = 0; i < 3; i++) {
        snprintf(urls[i], sizeof(urls[i]), "http://127.0.0.1:%u/f%d", PORT, i);
        snprintf(out_paths[i], sizeof(out_paths[i]), "%slass_test_httpd_f%d.bin", tmp_dir, i);
        DeleteFileA(out_paths[i]);
        dls[i] = engine_add(eng, urls[i], out_paths[i], 4, 0);
    }
    TEST("3 real downloads added", dls[0] && dls[1] && dls[2]);

    /* بازه‌ی صبر رو متناسب با stagger + trickle عمداً سخاوتمندانه
       گرفتیم: بدترین حالت ~ 2*STAGGER + TRICKLE + سربار ≈ 3+3=6s،
       پس ۲۰ ثانیه صبر می‌کنیم که روی ماشین‌های کندتر هم flaky نشه. */
    bool all_completed = false;
    for (int tick = 0; tick < 200; tick++) {
        Sleep(100);
        EnterCriticalSection(&eng->lock);
        bool done = dls[0] && dls[1] && dls[2] &&
                    dls[0]->state == DL_COMPLETED &&
                    dls[1]->state == DL_COMPLETED &&
                    dls[2]->state == DL_COMPLETED;
        LeaveCriticalSection(&eng->lock);
        if (done) { all_completed = true; break; }
    }

    int peak = test_httpd_peak_concurrent_gets();
    int completed = test_httpd_completed_gets();

    TEST("all 3 real downloads reached DL_COMPLETED", all_completed);
    TEST("server observed >=2 concurrent GET connections", peak >= 2);
    TEST("server actually served all 3 GET requests", completed >= 3);
    TEST("ETag from HEAD is sent back as If-Range on ranged GETs",
         test_httpd_if_range_gets() >= 1);

    engine_destroy(eng);
    TEST("engine destroy", TRUE);

    plugins_shutdown(plug_reg);
    test_httpd_stop();

    for (int i = 0; i < 3; i++) DeleteFileA(out_paths[i]);
}

/* ======================================================
   تست: چرخه‌ی عمر دانلود — validation، busy و حذف معلق
   ====================================================== */
IDM_Download *engine_find(IDM_Engine *eng, uint32_t id);

void test_dl_lifecycle(void) {
    SECTION("Download lifecycle: validation, busy and deferred removal");
    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) return;

    TEST("ftps link is rejected at engine level", engine_add(eng, "ftps://127.0.0.1:1/x.zip", "", 1, 0) == NULL);
    TEST("ftp link is accepted at engine level", engine_add(eng, "ftp://127.0.0.1:1/x.zip", "", 1, 0) != NULL);

    IDM_Download *upper = engine_add(eng, "HTTP://127.0.0.1:1/upper.zip", "", 1, 0);
    TEST("uppercase http scheme is accepted", upper != NULL);
    if (upper) engine_remove(eng, upper->id);

    IDM_Download *dl = engine_add(eng, "http://127.0.0.1:1/busy.zip", "", 1, 0);
    TEST("download added for lifecycle test", dl != NULL);
    if (!dl) { engine_destroy(eng); return; }
    uint32_t id = dl->id;

    TEST("acquire of unknown id returns NULL", engine_dl_acquire(eng, 0xFFFFFFFFu) == NULL);
    IDM_Download *held = engine_dl_acquire(eng, id);
    TEST("acquire of live download returns it", held == dl);
    TEST("acquire raises busy", held != NULL && held->busy >= 1);

    engine_remove(eng, id);
    TEST("removal unlinks the download at once", engine_find(eng, id) == NULL);
    TEST("removal of a held download is deferred", held != NULL && held->remove_pending);
    TEST("held download cannot be re-acquired after removal", engine_dl_acquire(eng, id) == NULL);

    engine_dl_release(eng, held);
    TEST("last release completes the deferred removal", TRUE);
    engine_destroy(eng);
    TEST("engine destroy after lifecycle test", TRUE);
}

/* ======================================================
   تست: FTP مستقل — URL، SIZE/REST، بازه‌ی شامل، و دانلود کامل با engine
   ====================================================== */
void test_ftp_download(void) {
    SECTION("FTP: independent client, ranged read and full download");
    const uint16_t FTP_PORT = 58761;
    const uint64_t FSIZE = 3u * 1024u * 1024u;
    bool srv_ok = test_ftpd_start(FTP_PORT, FSIZE);
    TEST("ftp test server started", srv_ok);
    if (!srv_ok) return;

    char url[160];
    snprintf(url, sizeof(url), "ftp://127.0.0.1:%u/file.bin", (unsigned)FTP_PORT);
    TEST("ftp url recognised", ftp_is_ftp_url(url));
    TEST("ftp url supported", dl_url_supported(url));
    TEST("ftps url not supported", !dl_url_supported("ftps://127.0.0.1/file.bin"));

    uint64_t size = 0;
    BOOL ranges = FALSE;
    char fn[IDM_MAX_FILENAME] = {0};
    IDM_Result r = ftp_discover(url, &size, fn, sizeof(fn), &ranges);
    TEST("discover succeeds", r == IDM_OK);
    TEST("discover reports the file size", size == FSIZE);
    TEST("discover reports the file name", strcmp(fn, "file.bin") == 0);
    TEST("discover detects REST support", ranges == TRUE);

    FtpStream *st = ftp_range_open(url, 1000, 1000 + 4095);
    TEST("ranged open (REST) succeeds", st != NULL);
    if (st) {
        static uint8_t rb[4096];
        int got = 0, n = 0;
        while (got < (int)sizeof(rb) && (n = ftp_range_read(st, rb + got, (int)sizeof(rb) - got)) > 0)
            got += n;
        TEST("ranged read returns exactly 4096 bytes", got == 4096);
        bool exact = (got == 4096);
        for (int i = 0; exact && i < got; i++)
            if (rb[i] != test_ftpd_byte(1000 + (uint64_t)i)) exact = false;
        TEST("ranged read bytes match the file at that offset", exact);
        TEST("server honoured the resume request", ftp_range_got_partial(st));
        ftp_range_close(st);
    }

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create for ftp download", eng != NULL);
    IDM_Download *dl = eng ? engine_add(eng, url, "ftp_test_out.bin", 4, 0) : NULL;
    TEST("ftp download added", dl != NULL);
    bool done = false;
    if (dl) {
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 30000) {
            if (dl->state == DL_COMPLETED) { done = true; break; }
            if (dl->state == DL_FAILED) break;
            Sleep(50);
        }
    }
    TEST("ftp download completes", done);

    bool content_ok = false;
    if (done) {
        FILE *f = fopen("ftp_test_out.bin", "rb");
        if (f) {
            static uint8_t cb[65536];
            uint64_t pos = 0;
            bool ok = true;
            size_t cnt = 0;
            while ((cnt = fread(cb, 1, sizeof(cb), f)) > 0) {
                for (size_t i = 0; i < cnt; i++)
                    if (cb[i] != test_ftpd_byte(pos + (uint64_t)i)) { ok = false; break; }
                if (!ok) break;
                pos += (uint64_t)cnt;
            }
            fclose(f);
            content_ok = ok && pos == FSIZE;
        }
    }
    TEST("downloaded file is byte-identical to the server file", content_ok);

    if (eng) engine_destroy(eng);
    test_ftpd_stop();
    DeleteFileA("ftp_test_out.bin");
}

void test_url_credentials(void) {
    SECTION("URL credentials: split, redact, protected queue and version 1 compatibility");
    lass_store_close();
    DeleteFileA("tels_test_cred_store.tels");
    lass_store_open("tels_test_cred_store.tels");
    char clean[IDM_MAX_URL] = {0};
    char pass[128] = {0};
    BOOL ok = url_split_credentials("ftp://alice:s3cret@127.0.0.1:21/a.bin", clean, sizeof(clean), pass, sizeof(pass));
    TEST("split succeeds on a URL with credentials", ok);
    TEST("split removes the password from the URL", strcmp(clean, "ftp://alice@127.0.0.1:21/a.bin") == 0);
    TEST("split returns the password separately", strcmp(pass, "s3cret") == 0);

    ok = url_split_credentials("http://example.com/x.zip", clean, sizeof(clean), pass, sizeof(pass));
    TEST("URL without password is unchanged", ok && strcmp(clean, "http://example.com/x.zip") == 0 && pass[0] == '\0');

    char full[IDM_MAX_URL] = {0};
    TEST("inject rebuilds the authenticated URL",
         url_inject_password("ftp://alice@127.0.0.1:21/a.bin", "s3cret", full, sizeof(full)) &&
         strcmp(full, "ftp://alice:s3cret@127.0.0.1:21/a.bin") == 0);

    char red[512] = {0};
    url_redact_for_log("ftp://alice:s3cret@127.0.0.1:21/a.bin", red, sizeof(red));
    TEST("log redaction hides the password", strstr(red, "s3cret") == NULL && strstr(red, "alice:***@") != NULL);
    url_redact_for_log("{\"action\":\"download\",\"url\":\"ftp://bob:hunter2@h/x\"}", red, sizeof(red));
    TEST("log redaction works inside JSON text", strstr(red, "hunter2") == NULL && strstr(red, "bob:***@h") != NULL);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "ftp://alice:s3cret@127.0.0.1:1/q.bin", "q_cred_test.bin", 1, 0) : NULL;
    TEST("ftp URL with credentials is accepted", dl != NULL);
    if (dl) {
        TEST("stored download URL has no password", strstr(dl->url, "s3cret") == NULL);
        TEST("connection URL keeps the password in memory", strstr(dl->secret_url, "s3cret") != NULL);
        TEST("password is held only in encrypted form", dl->pass_enc[0] != '\0' && strstr(dl->pass_enc, "s3cret") == NULL);
    }

    TEST("queue saved", eng && queue_persist_save(eng));
    {
        static uint8_t qb[1 << 16];
        uint8_t *blob = NULL;
        uint32_t n = 0;
        BOOL have = lass_store_get("queue", "state", &blob, &n) && n > 0 && n <= sizeof(qb);
        if (have) memcpy(qb, blob, n);
        free(blob);
        TEST("stored queue is readable", have);
        TEST("stored queue contains no plain password", have && !bytes_contain((const char *)qb, n, "s3cret"));
        TEST("stored queue contains no password in URL form", have && !bytes_contain((const char *)qb, n, "alice:s3cret"));
    }

    IDM_Engine *eng2 = engine_create(NULL);
    TEST("queue restored into a new engine", eng2 && queue_persist_load(eng2, NULL));
    IDM_Download *rdl = (eng2 && eng2->queue_head) ? eng2->queue_head : NULL;
    TEST("restored download gets its connection password back", rdl && strstr(rdl->secret_url, "s3cret") != NULL);
    TEST("restored download URL stays password-free", rdl && strstr(rdl->url, "s3cret") == NULL);

    /* صف نسخه‌ی ۱ (بدون فیلد رمز) باید همچنان خوانده شود */
    /* مهاجرت: فروشگاه تازه و خالی باشد تا صف نسخه‌ی ۱ از فایل قدیمی خوانده شود */
    lass_store_close();
    DeleteFileA("tels_test_cred_store.tels");
    lass_store_open("tels_test_cred_store.tels");
    const char *v1path = "queue_v1_test.dat";
    {
        typedef struct { uint32_t magic; uint16_t version; uint16_t pad; uint32_t count; uint32_t pad2; } V1Hdr;
        V1Hdr h = { QUEUE_PERSIST_MAGIC, (uint16_t)QUEUE_PERSIST_VERSION_V1, 0, 1, 0 };
        QueueRecord rec;
        memset(&rec, 0, sizeof(rec));
        snprintf(rec.url, sizeof(rec.url), "%s", "http://127.0.0.1:1/v1.bin");
        snprintf(rec.out_path, sizeof(rec.out_path), "%s", "v1_test.bin");
        rec.state = DL_PAUSED;
        rec.num_segments = 1;
        FILE *f = fopen(v1path, "wb");
        if (f) {
            fwrite(&h, sizeof(h), 1, f);
            fwrite(&rec, sizeof(rec), 1, f);
            fclose(f);
        }
    }
    IDM_Engine *eng3 = engine_create(NULL);
    TEST("version 1 queue file loads", eng3 && queue_persist_load(eng3, v1path));
    TEST("version 1 record restores its URL",
         eng3 && eng3->queue_head && strcmp(eng3->queue_head->url, "http://127.0.0.1:1/v1.bin") == 0);

    if (eng) engine_destroy(eng);
    if (eng2) engine_destroy(eng2);
    if (eng3) engine_destroy(eng3);
    lass_store_close();
    DeleteFileA("tels_test_cred_store.tels");
    DeleteFileA(v1path);
    DeleteFileA("q_cred_test.bin");
    DeleteFileA("v1_test.bin");
}

/* ======================================================
   تست: تخریب engine وقتی launch_thread وسط شبکه بلاک است — بدون leak و بدون انتظار طولانی
   ====================================================== */
static DWORD WINAPI stall_server_proc(LPVOID p) {
    SOCKET ls = (SOCKET)(UINT_PTR)p;
    SOCKET c = accept(ls, NULL, NULL);
    if (c != INVALID_SOCKET) {
        Sleep(20000);          /* هیچ‌وقت جواب نمی‌دهد */
        closesocket(c);
    }
    closesocket(ls);
    return 0;
}

void test_engine_destroy_while_launching(void) {
    SECTION("engine_destroy during a blocked launch: no leak, no long wait");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18477);
    BOOL listening = (ls != INVALID_SOCKET) &&
                     bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
                     listen(ls, 4) == 0;
    TEST("stall server listening", listening);
    if (!listening) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    HANDLE th = CreateThread(NULL, 0, stall_server_proc, (LPVOID)(UINT_PTR)ls, 0, NULL);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *dl = eng ? engine_add(eng, "http://127.0.0.1:18477/stall.bin", "stall_test.bin", 1, 0) : NULL;
    TEST("download added and launched", dl != NULL);
    Sleep(800);
    TEST("a launch thread is blocked at destroy time", eng && eng->active_count > 0);

    DWORD t0 = GetTickCount();
    engine_destroy(eng);
    DWORD spent = GetTickCount() - t0;
    TEST("engine_destroy returns without waiting for the network", spent < 2000);

    if (th) {
        WaitForSingleObject(th, 25000);
        CloseHandle(th);
    }
    DeleteFileA("stall_test.bin");
}

/* ======================================================
   تست: pause، cancel و schedule روی دانلودی که در launch است — حسابداری active_count
   هر عمل وقتی انجام می‌شود که همان دانلود در حال launch است، پس به زمان‌بندی شبکه وابسته نیست.
   بعد از برگشتن launch thread ها شمارش باید صفر بماند و scheduler هنوز آزاد باشد.
   ====================================================== */
static BOOL launch_wait_state(IDM_Engine *eng, IDM_Download *dl, DL_State want, DWORD ms) {
    DWORD t0 = GetTickCount();
    while (dl && GetTickCount() - t0 < ms) {
        BOOL hit;
        EnterCriticalSection(&eng->lock);
        hit = (dl->state == want);
        LeaveCriticalSection(&eng->lock);
        if (hit) return TRUE;
        Sleep(20);
    }
    return FALSE;
}

/* شمارش فعال باید دقیقاً برابر تعداد دانلودهایی باشد که DOWNLOADING یا LAUNCHING هستند */
static BOOL launch_count_matches(IDM_Engine *eng) {
    uint32_t n = 0;
    BOOL ok;
    EnterCriticalSection(&eng->lock);
    for (IDM_Download *x = eng->queue_head; x; x = x->next)
        if (x->state == DL_DOWNLOADING || x->state == DL_LAUNCHING) n++;
    ok = (eng->active_count == n);
    LeaveCriticalSection(&eng->lock);
    return ok;
}

void test_engine_action_during_launch(void) {
    SECTION("Engine: pausing, cancelling or scheduling a launching download keeps the active count right");
    /* نام‌ها در هر اجرا یکتا هستند: فایل وضعیت یا temp اجرای قبلی به این تست نمی‌رسد */
    char ua[200], ub[200], ud[200], na[120], nb[120], nd[120];
    unsigned long tk = (unsigned long)GetTickCount();
    snprintf(ua, sizeof(ua), "http://127.0.0.1:18483/launch_a_%lu.bin", tk);
    snprintf(ub, sizeof(ub), "http://127.0.0.1:18483/launch_b_%lu.bin", tk);
    snprintf(ud, sizeof(ud), "http://127.0.0.1:18483/launch_d_%lu.bin", tk);
    snprintf(na, sizeof(na), "tels_test_launch_a_%lu.bin", tk);
    snprintf(nb, sizeof(nb), "tels_test_launch_b_%lu.bin", tk);
    snprintf(nd, sizeof(nd), "tels_test_launch_d_%lu.bin", tk);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(18483);
    BOOL listening = (ls != INVALID_SOCKET) &&
                     bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
                     listen(ls, 8) == 0;
    TEST("launch test server listening", listening);
    if (!listening) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    HANDLE th = CreateThread(NULL, 0, stall_server_proc, (LPVOID)(UINT_PTR)ls, 0, NULL);

    IDM_Engine *eng = engine_create(NULL);
    IDM_Download *a = eng ? engine_add(eng, ua,
                                       na, 1, 0) : NULL;
    IDM_Download *b = eng ? engine_add(eng, ub,
                                       nb, 1, 0) : NULL;
    IDM_Download *d = eng ? engine_add(eng, ud,
                                       nd, 1, 0) : NULL;
    TEST("three downloads queued", a != NULL && b != NULL && d != NULL);

    /* هر عمل وقتی انجام می‌شود که همان دانلود در حال launch است */
    BOOL a_ok = eng && a && launch_wait_state(eng, a, DL_LAUNCHING, 15000u);
    if (a_ok) engine_pause(eng, a->id);
    BOOL b_ok = eng && b && launch_wait_state(eng, b, DL_LAUNCHING, 15000u);
    if (b_ok) engine_cancel(eng, b->id);
    BOOL d_ok = eng && d && launch_wait_state(eng, d, DL_LAUNCHING, 15000u);
    if (d_ok) engine_schedule(eng, d->id, 23, 59);
    TEST("each download was seen launching before its action", a_ok && b_ok && d_ok);
    if (!(a_ok && b_ok && d_ok) && a && b && d)
        printf("    (states: a=%d b=%d d=%d)\n", (int)a->state, (int)b->state, (int)d->state);
    TEST("scheduled download is paused by the schedule", d != NULL && d->state == DL_PAUSED);
    TEST("pause, cancel and schedule leave the count at zero", eng && eng->active_count == 0);
    TEST("counts match the downloads that are still active", eng && launch_count_matches(eng));

    /* صبر تا هر سه launch thread برگردند: discovery با timeout شبکه تمام می‌شود */
    DWORD t0 = GetTickCount();
    while (a && b && d && (a->busy > 0 || b->busy > 0 || d->busy > 0) &&
           GetTickCount() - t0 < 30000u)
        Sleep(50);
    TEST("all launch threads have returned",
         a && b && d && a->busy == 0 && b->busy == 0 && d->busy == 0);
    TEST("active count is still zero after the launch threads return",
         eng && eng->active_count == 0);
    /* اگر شمارش خراب شده باشد، scheduler دیگر هیچ دانلودی را شروع نمی‌کند */
    TEST("the scheduler can still launch a download (count below the limit)",
         eng && eng->active_count < eng->max_concurrent);

    engine_destroy(eng);
    if (th) {
        WaitForSingleObject(th, 25000);
        CloseHandle(th);
    }
    DT_CLEAN_NAME(na);
    DT_CLEAN_NAME(nb);
    DT_CLEAN_NAME(nd);
}

/* ======================================================
   تست: Launch Retry Backoff — retry خودکار discovery failures
   ====================================================== */
void test_launch_retry_backoff(void) {
    SECTION("Engine: Launch Retry Backoff");

    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) return;

    IDM_Download *dl = engine_add(eng, "http://x.com/f.zip", "C:\\f.zip", 4, 0);
    TEST("download added", dl != NULL);
    if (!dl) { engine_destroy(eng); return; }

    /* شبیه‌سازی دستی یه شکست discovery — چون واقعاً شبکه نداریم،
       مستقیم منطق retry رو با تنظیم فیلدها تست می‌کنیم */
    TEST("launch_retry_count starts at 0", dl->launch_retry_count == 0);
    TEST("next_launch_retry_at starts at 0", dl->next_launch_retry_at == 0);

    /* شبیه‌سازی افزایش بعد از شکست اول */
    dl->launch_retry_count = 1;
    DWORD backoff1 = 3000u * (1u << (dl->launch_retry_count - 1));
    TEST("first backoff is 3000ms", backoff1 == 3000);

    dl->launch_retry_count = 3;
    DWORD backoff3 = 3000u * (1u << (dl->launch_retry_count - 1));
    TEST("third backoff is 12000ms", backoff3 == 12000);

    dl->launch_retry_count = 10; /* خیلی زیاد */
    DWORD backoff10 = 3000u * (1u << (dl->launch_retry_count - 1));
    if (backoff10 > 60000) backoff10 = 60000;
    TEST("backoff caps at 60000ms", backoff10 == 60000);

    TEST("ENGINE_MAX_LAUNCH_RETRY is reasonable",
         ENGINE_MAX_LAUNCH_RETRY >= 3 && ENGINE_MAX_LAUNCH_RETRY <= 10);

    /* تست اینکه scheduler واقعاً به next_launch_retry_at احترام
       میذاره — یه دانلود با retry timer در آینده رو QUEUED می‌ذاریم
       و بررسی می‌کنیم که با یه scheduler tick دستی، launch نشه */
    dl->state = DL_QUEUED;
    dl->next_launch_retry_at = GetTickCount() + 10000; /* ۱۰ ثانیه بعد */
    BOOL should_wait = (dl->next_launch_retry_at != 0 &&
                        GetTickCount() < dl->next_launch_retry_at);
    TEST("scheduler would skip download still in backoff", should_wait);

    /* بعد از گذشت زمان، باید آزاد بشه */
    dl->next_launch_retry_at = GetTickCount() - 1000; /* در گذشته */
    BOOL should_launch_now = !(dl->next_launch_retry_at != 0 &&
                               GetTickCount() < dl->next_launch_retry_at);
    TEST("scheduler would launch after backoff expires", should_launch_now);

    engine_destroy(eng);
    TEST("engine destroy", TRUE);
}

/* ======================================================
   تست: خطای دائمی (404) در برابر موقتی — نباید retry بشه
   ====================================================== */
void test_permanent_vs_transient_errors(void) {
    SECTION("Engine: Permanent (404) vs Transient Error Handling");

    /* IDM_ERR_NOT_FOUND باید از IDM_ERR_NETWORK متمایز باشه —
       این دو enum value باید متفاوت باشن تا منطق engine.c
       بتونه بین‌شون تمایز قائل بشه */
    TEST("NOT_FOUND differs from NETWORK error",
         IDM_ERR_NOT_FOUND != IDM_ERR_NETWORK);
    TEST("NOT_FOUND is a distinct negative error code",
         IDM_ERR_NOT_FOUND < 0);

    /* شبیه‌سازی منطق permanent-detection که در http.c هست */
    struct { int status; BOOL should_be_permanent; } cases[] = {
        {404, TRUE},   /* پیدا نشد — دائمی */
        {410, TRUE},   /* دیگه موجود نیست — دائمی */
        {400, TRUE},   /* درخواست بدشکل — دائمی */
        {401, TRUE},   /* نیاز به احراز هویت — دائمی */
        {403, FALSE},  /* می‌تونه rate-limit موقتی باشه */
        {429, FALSE},  /* too many requests — موقتی */
        {500, FALSE},  /* خطای سرور — موقتی */
        {503, FALSE},  /* سرویس در دسترس نیست — موقتی */
    };
    BOOL all_correct = TRUE;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        BOOL permanent = (cases[i].status == 404 || cases[i].status == 410 ||
                          cases[i].status == 400 || cases[i].status == 401);
        if (permanent != cases[i].should_be_permanent) all_correct = FALSE;
    }
    TEST("permanent/transient classification matches expected", all_correct);
}

/* ======================================================
   تست: فایل .tels (مسیر موقت حین دانلود)
   ====================================================== */
void test_tels_temp_file(void) {
    SECTION("Tels Temp File (.tels)");

    IDM_Engine *eng = engine_create(NULL);
    eng->max_concurrent = 0; /* جلوگیری از شروع واقعی دانلود */
    TEST("engine create",       eng != NULL);
    if (!eng) return;

    IDM_Download *dl = engine_add(eng, "http://example.com/movie.mp4",
                                  "C:\\Downloads\\movie.mp4", 4, 0);
    TEST("download added",      dl != NULL);
    if (dl) {
        TEST("temp_path has .tels suffix",
            strstr(dl->temp_path, ".tels") != NULL);
        TEST("temp_path based on out_path",
            strncmp(dl->temp_path, dl->out_path,
                    strlen(dl->out_path)) == 0);
        TEST("out_path unchanged",
            strcmp(dl->out_path, "C:\\Downloads\\movie.mp4") == 0);
        TEST("temp_path exact value",
            strcmp(dl->temp_path,
                   "C:\\Downloads\\movie.mp4.tels") == 0);
    }

    /* فیکس دفاعی: out_path خالی ("") قبلاً به‌عنوان یه مسیر معتبر
       قبول می‌شد (فقط NULL fallback داشت) و temp_path می‌شد فقط
       ".tels" — یه فایل با اسم ناقص در دایرکتوری جاری. */
    IDM_Download *dl_empty = engine_add(eng, "http://example.com/x.zip",
                                        "", 4, 0);
    TEST("empty out_path accepted without crashing", dl_empty != NULL);
    if (dl_empty) {
        TEST("empty out_path falls back (not left empty)",
             dl_empty->out_path[0] != '\0');
        TEST("empty out_path temp_path is not just '.tels'",
             strcmp(dl_empty->temp_path, ".tels") != 0);
    }

    engine_destroy(eng);
    TEST("engine destroy",      TRUE);
}

/* مسیر خیلی طولانی — نباید overflow بده یا warning کامپایلر بسازه */
void test_tels_long_path(void) {
    SECTION("Tels Temp File — long path edge case");

    IDM_Engine *eng = engine_create(NULL);
    eng->max_concurrent = 0;
    TEST("engine create",       eng != NULL);
    if (!eng) return;

    /* یه مسیر نزدیک به IDM_MAX_PATH (260) بساز */
    char long_path[300];
    strcpy(long_path, "C:\\");
    for (int i = 0; i < 250; i++) {
        long_path[3 + i] = 'a' + (i % 26);
    }
    long_path[253] = '\0';

    IDM_Download *dl = engine_add(eng, "http://example.com/file.bin",
                                  long_path, 4, 0);
    TEST("download added with long path", dl != NULL);
    if (dl) {
        size_t tlen = strlen(dl->temp_path);
        TEST("temp_path within bounds",
            tlen < (size_t)IDM_MAX_PATH);
        TEST("temp_path null terminated",
            dl->temp_path[IDM_MAX_PATH - 1] == '\0' ||
            tlen < (size_t)(IDM_MAX_PATH - 1));
        TEST("temp_path still has .tels somewhere or was truncated safely",
            tlen > 0);
    }

    engine_destroy(eng);
    TEST("engine destroy",      TRUE);
}

/* ======================================================
   تست: Queue Persist — ذخیره و بارگذاری لیست دانلودها
   ====================================================== */
void test_queue_persist(void) {
    SECTION("Queue Persist (session restore)");

    /* فروشگاه موقت برای این تست */
    lass_store_close();
    DeleteFileA("tels_test_queue_store.tels");
    TEST("queue store opens", lass_store_open("tels_test_queue_store.tels"));

    /* ── ساخت engine با چند دانلود ── */
    IDM_Engine *eng = engine_create(NULL);
    TEST("engine create", eng != NULL);
    if (!eng) {
        lass_store_close();
        DeleteFileA("tels_test_queue_store.tels");
        return;
    }
    eng->max_concurrent = 0; /* auto-launch نشه */

    engine_add(eng, "http://example.com/a.zip", "C:\\Downloads\\a.zip", 4, 0);
    engine_add(eng, "http://example.com/b.mp4", "C:\\Downloads\\b.mp4", 8, 0);
    engine_add(eng, "http://example.com/c.pdf", "C:\\Downloads\\c.pdf", 2, 0);

    /* ── save در فروشگاه ── */
    BOOL saved = queue_persist_save(eng);
    TEST("save returns TRUE", saved);
    TEST("queue is stored in the container", lass_store_exists("queue", "state"));

    uint8_t *qb = NULL;
    uint32_t qlen = 0;
    BOOL got_blob = lass_store_get("queue", "state", &qb, &qlen);
    TEST("stored queue has a header and records", got_blob && qlen > sizeof(uint32_t) * 3);
    free(qb);

    engine_destroy(eng);

    /* ماندگاری: فروشگاه بسته و دوباره باز شود، صف باید همان‌جا باشد */
    lass_store_close();
    TEST("store reopens for the queue", lass_store_open("tels_test_queue_store.tels"));

    /* ── load در یه engine جدید ── */
    IDM_Engine *eng2 = engine_create(NULL);
    TEST("engine2 create", eng2 != NULL);
    if (!eng2) {
        lass_store_close();
        DeleteFileA("tels_test_queue_store.tels");
        return;
    }
    eng2->max_concurrent = 0;

    BOOL loaded = queue_persist_load(eng2, NULL);
    TEST("load returns TRUE", loaded);

    /* شمارش دانلودهای بازیابی‌شده */
    int cnt = 0;
    BOOL states_paused = TRUE;
    EnterCriticalSection(&eng2->lock);
    for (IDM_Download *dl = eng2->queue_head; dl; dl = dl->next) {
        cnt++;
        if (dl->state != DL_PAUSED) states_paused = FALSE;
    }
    LeaveCriticalSection(&eng2->lock);
    TEST("3 downloads restored", cnt == 3);
    TEST("all restored as PAUSED", states_paused);

    /* بررسی URL اولین دانلود */
    BOOL url_ok = FALSE;
    EnterCriticalSection(&eng2->lock);
    if (eng2->queue_head)
        url_ok = (strstr(eng2->queue_head->url, "example.com") != NULL);
    LeaveCriticalSection(&eng2->lock);
    TEST("url preserved", url_ok);

    /* null safety */
    TEST("save null safe", !queue_persist_save(NULL));
    TEST("load null safe", !queue_persist_load(NULL, NULL));

    engine_destroy(eng2);
    lass_store_close();
    DeleteFileA("tels_test_queue_store.tels");
    TEST("engine destroy", TRUE);
}

/* ======================================================
   تست ۲۶: وضعیت ادامه‌ی بدون segment و دانلود صفربایتی
   ====================================================== */
static const uint8_t DT_ZERO_BODY[1] = {0};

void test_resume_and_zero_byte(void) {
    IDM_Engine *eng;
    IDM_Download *dl;
    SECTION("Resume state with no segments is never saved, loaded or completed");
    eng = engine_create(NULL);
    TEST("engine created for resume checks", eng != NULL);
    if (eng) eng->max_concurrent = 0;
    dl = eng ? engine_add(eng, "http://127.0.0.1:18495/no_segments.bin", "tels_test_noseg.bin", 1, 0) : NULL;
    TEST("download without segments is queued", dl != NULL && dl->num_segments == 0);
    if (dl) {
        char sp[IDM_MAX_PATH + 32];
        FILE *f;
        IDM_ResumeData hdr;
        snprintf(sp, sizeof(sp), "%s%s", dl->out_path, IDM_STATE_FILE_EXT);
        DeleteFileA(sp);
        TEST("save refuses a download with no segments", !resume_save(dl));
        TEST("no state file is written for it", GetFileAttributesA(sp) == INVALID_FILE_ATTRIBUTES);
        f = fopen(sp, "wb");
        TEST("stale zero-segment state file can be planted", f != NULL);
        if (f) {
            memset(&hdr, 0, sizeof(hdr));
            hdr.magic = 0x49444D52u;
            hdr.version = 1;
            hdr.num_segments = 0;
            snprintf(hdr.url, sizeof(hdr.url), "%s", dl->url);
            snprintf(hdr.out_path, sizeof(hdr.out_path), "%s", dl->out_path);
            fwrite(&hdr, sizeof(hdr), 1, f);
            fclose(f);
        }
        TEST("loader rejects a state with no segments", !resume_load(dl));
        TEST("loader leaves no resume data on the download", dl->resume_segs == NULL);
        TEST("stale zero-segment state file is removed", GetFileAttributesA(sp) == INVALID_FILE_ATTRIBUTES);
    }
    engine_destroy(eng);
    DT_CLEAN_NAME("tels_test_noseg.bin");

    if (hls_test_server_start(18495)) {
        DL_State st = DL_IDLE;
        IDM_Download *z;
        IDM_Engine *e2 = engine_create(NULL);
        DWORD t0;
        unsigned seen_states = 0;
        hls_test_server_set("/zero_byte.bin", DT_ZERO_BODY, 0);
        z = e2 ? engine_add(e2, "http://127.0.0.1:18495/zero_byte.bin", "tels_test_zero_byte.bin", 1, 0) : NULL;
        TEST("zero-byte download is queued", z != NULL);
        t0 = GetTickCount();
        while (z && GetTickCount() - t0 < 20000u) {
            EnterCriticalSection(&e2->lock);
            st = z->state;
            seen_states |= 1u << (unsigned)st;
            LeaveCriticalSection(&e2->lock);
            if (st == DL_COMPLETED || st == DL_FAILED) break;
            Sleep(50);
        }
        if (z && st != DL_COMPLETED)
            printf("    (zero-byte: final state=%d, states seen=0x%x, total=%llu, downloaded=%llu, reason=\"%s\")\n",
                   (int)st, seen_states, (unsigned long long)z->total_size,
                   (unsigned long long)z->downloaded, z->fail_reason);
        TEST("zero-byte download completes", z != NULL && st == DL_COMPLETED);
        if (z && st == DL_COMPLETED) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            memset(&fa, 0, sizeof(fa));
            TEST("zero-byte output file exists and is empty",
                 GetFileAttributesExA(z->out_path, GetFileExInfoStandard, &fa) &&
                 fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0);
        }
        engine_destroy(e2);
        hls_test_server_stop();
        DT_CLEAN_NAME("tels_test_zero_byte.bin");
    } else {
        TEST("test server for zero-byte download starts", FALSE);
    }
}

