/* storage: container, store and migration tests (tests/test_storage.c) */
#include "test_common.h"

/* ======================================================
   تست: انتقال داده‌های نسخه‌ی قدیمی (پوشه‌ی قدیمی → Lass)
   ====================================================== */
static void mig_write(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int mig_read_line(const char *path, char *out, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = (fgets(out, (int)sz, f) != NULL);
    fclose(f);
    return ok;
}

void test_legacy_migration(void) {
    SECTION("Legacy data migration (old folder -> Lass)");
    CreateDirectoryA("mig_test", NULL);
    CreateDirectoryA("mig_test\\old_states", NULL);

    mig_write("mig_test\\old_cookies.txt", "old-data\n");
    int c1 = legacy_copy_file_if_missing("mig_test\\old_cookies.txt",
                                         "mig_test\\new\\cookies.txt");
    TEST("single file is copied (creating the new folder)", c1 == 1);
    char buf[64] = {0};
    TEST("copied content is identical",
         mig_read_line("mig_test\\new\\cookies.txt", buf, sizeof(buf)) &&
         strcmp(buf, "old-data\n") == 0);

    mig_write("mig_test\\old_cookies.txt", "changed\n");
    int c2 = legacy_copy_file_if_missing("mig_test\\old_cookies.txt",
                                         "mig_test\\new\\cookies.txt");
    TEST("existing destination is never overwritten", c2 == 0);
    memset(buf, 0, sizeof(buf));
    mig_read_line("mig_test\\new\\cookies.txt", buf, sizeof(buf));
    TEST("destination content kept after a second call", strcmp(buf, "old-data\n") == 0);
    TEST("legacy source is left intact",
         GetFileAttributesA("mig_test\\old_cookies.txt") != INVALID_FILE_ATTRIBUTES);
    TEST("missing legacy file is a no-op",
         legacy_copy_file_if_missing("mig_test\\nope.txt", "mig_test\\x.txt") == 0);

    mig_write("mig_test\\old_states\\a.dat", "A");
    mig_write("mig_test\\old_states\\b.dat", "B");
    mig_write("mig_test\\old_states\\skip.txt", "X");
    int d1 = legacy_copy_dat_dir_if_missing("mig_test\\old_states", "mig_test\\new_states");
    TEST("only .dat state files are migrated", d1 == 2);
    TEST("non-.dat file is not migrated",
         GetFileAttributesA("mig_test\\new_states\\skip.txt") == INVALID_FILE_ATTRIBUTES);
    int d2 = legacy_copy_dat_dir_if_missing("mig_test\\old_states", "mig_test\\new_states");
    TEST("second directory migration copies nothing", d2 == 0);

    DeleteFileA("mig_test\\old_cookies.txt");
    DeleteFileA("mig_test\\new\\cookies.txt");
    DeleteFileA("mig_test\\old_states\\a.dat");
    DeleteFileA("mig_test\\old_states\\b.dat");
    DeleteFileA("mig_test\\old_states\\skip.txt");
    DeleteFileA("mig_test\\new_states\\a.dat");
    DeleteFileA("mig_test\\new_states\\b.dat");
    RemoveDirectoryA("mig_test\\new");
    RemoveDirectoryA("mig_test\\new_states");
    RemoveDirectoryA("mig_test\\old_states");
    RemoveDirectoryA("mig_test");
}

/* ======================================================
   تست: رمز FTP — جداسازی از URL، پوشاندن در لاگ، صف رمزشده و سازگاری با صف نسخه‌ی ۱
   ====================================================== */
BOOL bytes_contain(const char *data, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return FALSE;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(data + i, needle, m) == 0) return TRUE;
    return FALSE;
}

/* ======================================================
   تست: کانتینر .tels — سیستم فایل مجازی، commit، بازیابی و همروندی
   ====================================================== */
static uint64_t tels_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static BOOL tels_make_sample(const char *path, TelsId *afile_out, TelsId *docs_out) {
    DeleteFileA(path);
    TelsContainer *c = tels_open(path, TRUE);
    if (!c) return FALSE;
    TelsId docs = 0, afile = 0;
    BOOL ok = tels_create(c, TELS_ROOT, "docs", TELS_TYPE_DIR, &docs) &&
              tels_create(c, docs, "a.txt", TELS_TYPE_FILE, &afile) &&
              tels_write(c, afile, 0, "hello", 5) &&
              tels_commit(c);
    tels_close(c);
    if (afile_out) *afile_out = afile;
    if (docs_out) *docs_out = docs;
    return ok;
}

typedef struct { TelsContainer *c; TelsId id; int t; } TelsWriterArg;

static DWORD WINAPI tels_writer_proc(LPVOID p) {
    TelsWriterArg *a = (TelsWriterArg *)p;
    uint8_t blk[512];
    memset(blk, (int)('A' + a->t), sizeof(blk));
    for (int i = 0; i < 200; i++) {
        uint64_t off = (uint64_t)(i * 4 + a->t) * 512u;
        tels_write(a->c, a->id, off, blk, sizeof(blk));
    }
    return 0;
}

void test_tels_container(void) {
    SECTION("Container .tels: virtual filesystem, commit and recovery");
    const char *P = "tels_test_main.tels";
    DeleteFileA(P);
    TEST("missing container is not opened without create", tels_open("tels_test_absent.tels", FALSE) == NULL);
    TelsContainer *c = tels_open(P, TRUE);
    TEST("container created", c != NULL);
    if (!c) return;

    TelsId docs = 0, afile = 0;
    TEST("directory created at the root", tels_create(c, TELS_ROOT, "docs", TELS_TYPE_DIR, &docs));
    TEST("file created inside a directory", tels_create(c, docs, "a.txt", TELS_TYPE_FILE, &afile));
    TEST("duplicate name in one directory is rejected (case-insensitive)",
         !tels_create(c, docs, "A.TXT", TELS_TYPE_FILE, NULL));
    TEST("write hello", tels_write(c, afile, 0, "hello", 5));
    char rb[64] = {0};
    TEST("read back hello", tels_read(c, afile, 0, rb, 5) == 5 && memcmp(rb, "hello", 5) == 0);
    uint64_t sz = 0;
    TEST("file size is 5", tels_size(c, afile, &sz) && sz == 5);
    TelsId found = 0;
    TEST("lookup finds the file by name", tels_lookup(c, docs, "a.txt", &found) && found == afile);
    TEST("a file cannot contain another entry", !tels_create(c, afile, "x", TELS_TYPE_FILE, NULL));

    TelsId s = 0;
    tels_create(c, TELS_ROOT, "sparse.bin", TELS_TYPE_FILE, &s);
    TEST("write at a later offset first", tels_write(c, s, 10, "world", 5));
    TEST("then write at offset 0", tels_write(c, s, 0, "hello", 5));
    char sb[32] = {0};
    TEST("out-of-order writes read back with a zero gap",
         tels_read(c, s, 0, sb, 15) == 15 && memcmp(sb, "hello\0\0\0\0\0world", 15) == 0);
    TEST("read at the end returns 0", tels_read(c, s, 15, sb, 4) == 0);
    TEST("read across the end returns the available bytes",
         tels_read(c, s, 12, sb, 10) == 3 && memcmp(sb, "rld", 3) == 0);
    TEST("overwrite inside an existing extent", tels_write(c, s, 2, "XYZ", 3));
    char ob[16] = {0};
    TEST("overwritten content reads back correctly",
         tels_read(c, s, 0, ob, 15) == 15 && memcmp(ob, "heXYZ\0\0\0\0\0world", 15) == 0);

    TelsId cur = TELS_ROOT, deep = 0;
    BOOL nest_ok = TRUE;
    for (int i = 1; i <= 16; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "d%d", i);
        TelsId nx = 0;
        if (!tels_create(c, cur, nm, TELS_TYPE_DIR, &nx)) { nest_ok = FALSE; break; }
        cur = nx;
    }
    TEST("nesting 16 containers deep succeeds", nest_ok);
    TEST("the 17th nested container is rejected", !tels_create(c, cur, "too_deep", TELS_TYPE_DIR, NULL));
    TEST("resolve creates a nested path", tels_resolve(c, "x/y/z", TRUE, &deep) && deep != 0);
    TelsId got = 0;
    TEST("resolve finds the same nested path again", tels_resolve(c, "x/y/z", FALSE, &got) && got == deep);

    TEST("empty name is rejected", !tels_create(c, TELS_ROOT, "", TELS_TYPE_FILE, NULL));
    TEST("name with a separator is rejected", !tels_create(c, TELS_ROOT, "a/b", TELS_TYPE_FILE, NULL));
    TEST("name with a colon is rejected", !tels_create(c, TELS_ROOT, "a:b", TELS_TYPE_FILE, NULL));
    TEST("'..' is rejected", !tels_create(c, TELS_ROOT, "..", TELS_TYPE_FILE, NULL));
    char longname[300];
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    TEST("name longer than the limit is rejected", !tels_create(c, TELS_ROOT, longname, TELS_TYPE_FILE, NULL));

    TelsId kids[32];
    TEST("list shows the file inside docs",
         tels_list(c, docs, kids, 32) == 1 && kids[0] == afile);

    TEST("commit succeeds", tels_commit(c));
    tels_close(c);

    c = tels_open(P, FALSE);
    TEST("reopen after a clean close", c != NULL);
    if (c) {
        TelsId f2 = 0;
        TEST("file found after reopen", tels_lookup(c, docs, "a.txt", &f2) && f2 == afile);
        char rb2[16] = {0};
        TEST("content survives reopen", tels_read(c, afile, 0, rb2, 5) == 5 && memcmp(rb2, "hello", 5) == 0);
        char sb2[32] = {0};
        TEST("sparse content survives reopen",
             tels_read(c, s, 0, sb2, 15) == 15 && memcmp(sb2, "heXYZ\0\0\0\0\0world", 15) == 0);
        TelsId got2 = 0;
        TEST("nested path survives reopen", tels_resolve(c, "x/y/z", FALSE, &got2) && got2 == deep);
    }

    TelsId post = 0;
    TEST("file created after the commit", c && tels_create(c, docs, "after.txt", TELS_TYPE_FILE, &post));
    TEST("data written after the commit", c && tels_write(c, post, 0, "unsaved", 7));
    tels_abort(c);
    c = tels_open(P, FALSE);
    TEST("reopen after an abort without commit", c != NULL);
    if (c) {
        TelsId p2 = 0;
        char pb[16] = {0};
        TEST("complete records written after the last commit are recovered",
             tels_lookup(c, docs, "after.txt", &p2) && tels_read(c, p2, 0, pb, 7) == 7 &&
             memcmp(pb, "unsaved", 7) == 0);
        TEST("commit before the torn-write test", tels_commit(c));
        TelsId tf = 0;
        tels_create(c, docs, "torn.txt", TELS_TYPE_FILE, &tf);
        tels_write(c, tf, 0, "0123456789ABCDEF", 16);
        tels_abort(c);
    }
    {
        HANDLE hf = CreateFileA(P, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz2, cut;
            GetFileSizeEx(hf, &sz2);
            cut.QuadPart = sz2.QuadPart - 8;
            SetFilePointerEx(hf, cut, NULL, FILE_BEGIN);
            SetEndOfFile(hf);
            CloseHandle(hf);
        }
    }
    c = tels_open(P, FALSE);
    TEST("reopen after a torn tail", c != NULL);
    if (c) {
        TelsId t2 = 0;
        uint64_t tsz = 99;
        TEST("the torn record is dropped: the file exists and is empty",
             tels_lookup(c, docs, "torn.txt", &t2) && tels_size(c, t2, &tsz) && tsz == 0);
        TelsId a2 = 0;
        char ab[8] = {0};
        TEST("data before the torn record survives",
             tels_lookup(c, docs, "after.txt", &a2) && tels_read(c, a2, 0, ab, 7) == 7 && memcmp(ab, "unsaved", 7) == 0);
        TEST("writes work after recovery", tels_write(c, t2, 0, "ok", 2));
        tels_close(c);
    }

    /* superblock جدیدتر خراب: باید superblock قبلی و replay کافی باشد */
    const char *P2 = "tels_test_sb.tels";
    TelsId a3 = 0, d3 = 0;
    TEST("sample container for superblock damage", tels_make_sample(P2, &a3, &d3));
    c = tels_open(P2, FALSE);
    if (c) {
        TEST("commit again so two superblocks hold different generations", tels_commit(c));
        tels_abort(c);
    }
    {
        HANDLE hf = CreateFileA(P2, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            uint8_t sa[16], sbb[16];
            DWORD got_a = 0, got_b = 0;
            SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            ReadFile(hf, sa, sizeof(sa), &got_a, NULL);
            SetFilePointer(hf, 4096, NULL, FILE_BEGIN);
            ReadFile(hf, sbb, sizeof(sbb), &got_b, NULL);
            DWORD victim = (tels_le64(sa + 12) > tels_le64(sbb + 12)) ? 0 : 4096;
            uint8_t junk = 0xFF;
            SetFilePointer(hf, victim + 13, NULL, FILE_BEGIN);
            WriteFile(hf, &junk, 1, &got_a, NULL);
            CloseHandle(hf);
        }
    }
    c = tels_open(P2, FALSE);
    TEST("container opens with the newest superblock damaged", c != NULL);
    if (c) {
        char hb[8] = {0};
        TelsId f3 = 0;
        TEST("content survives a damaged newest superblock",
             tels_lookup(c, d3, "a.txt", &f3) && tels_read(c, f3, 0, hb, 5) == 5 && memcmp(hb, "hello", 5) == 0);
        tels_close(c);
    }

    /* index خراب: replay کامل از ابتدا باید همه‌ی داده‌ها را بازسازی کند */
    const char *P3 = "tels_test_idx.tels";
    TelsId a4 = 0, d4 = 0;
    TEST("sample container for index damage", tels_make_sample(P3, &a4, &d4));
    {
        HANDLE hf = CreateFileA(P3, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            uint8_t sa[32], sbb[32];
            DWORD got = 0;
            SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            ReadFile(hf, sa, sizeof(sa), &got, NULL);
            SetFilePointer(hf, 4096, NULL, FILE_BEGIN);
            ReadFile(hf, sbb, sizeof(sbb), &got, NULL);
            BOOL a_newer = tels_le64(sa + 12) > tels_le64(sbb + 12);
            uint64_t idx_pos = a_newer ? tels_le64(sa + 20) : tels_le64(sbb + 20);
            uint8_t junk = 0xAA;
            SetFilePointer(hf, (LONG)(idx_pos + 72), NULL, FILE_BEGIN);
            WriteFile(hf, &junk, 1, &got, NULL);
            CloseHandle(hf);
        }
    }
    c = tels_open(P3, FALSE);
    TEST("container opens with a damaged index record", c != NULL);
    if (c) {
        char hb2[8] = {0};
        TelsId f4 = 0;
        TEST("replay from the start restores the content",
             tels_lookup(c, d4, "a.txt", &f4) && tels_read(c, f4, 0, hb2, 5) == 5 && memcmp(hb2, "hello", 5) == 0);
        tels_close(c);
    }

    /* هر دو superblock خراب: replay از ابتدا */
    const char *P4 = "tels_test_both.tels";
    TelsId a5 = 0, d5 = 0;
    TEST("sample container for double superblock damage", tels_make_sample(P4, &a5, &d5));
    {
        HANDLE hf = CreateFileA(P4, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            uint8_t junk[8];
            memset(junk, 0xEE, sizeof(junk));
            DWORD got = 0;
            SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            WriteFile(hf, junk, sizeof(junk), &got, NULL);
            SetFilePointer(hf, 4096, NULL, FILE_BEGIN);
            WriteFile(hf, junk, sizeof(junk), &got, NULL);
            CloseHandle(hf);
        }
    }
    c = tels_open(P4, FALSE);
    TEST("container opens with both superblocks damaged", c != NULL);
    if (c) {
        char hb3[8] = {0};
        TelsId f5 = 0;
        TEST("replay restores the content with no superblock",
             tels_lookup(c, d5, "a.txt", &f5) && tels_read(c, f5, 0, hb3, 5) == 5 && memcmp(hb3, "hello", 5) == 0);
        tels_close(c);
    }

    /* حذف یک پوشه با تمام زیرمجموعه‌ها، و ماندگاری حذف */
    const char *P5 = "tels_test_del.tels";
    DeleteFileA(P5);
    c = tels_open(P5, TRUE);
    if (c) {
        TelsId tree = 0, sub = 0, leaf = 0;
        tels_create(c, TELS_ROOT, "tree", TELS_TYPE_DIR, &tree);
        tels_create(c, tree, "sub", TELS_TYPE_DIR, &sub);
        tels_create(c, sub, "leaf.bin", TELS_TYPE_FILE, &leaf);
        tels_write(c, leaf, 0, "data", 4);
        TEST("subtree deleted", tels_delete(c, tree));
        TEST("deleted subtree is gone", !tels_lookup(c, TELS_ROOT, "tree", NULL));
        TEST("the root itself cannot be deleted", !tels_delete(c, TELS_ROOT));
        tels_close(c);
        c = tels_open(P5, FALSE);
        TEST("deletion survives reopen", c && !tels_lookup(c, TELS_ROOT, "tree", NULL));
        if (c) tels_close(c);
    }

    /* همروندی: چهار thread روی بازه‌های جدا می‌نویسند (مثل segment های موازی) */
    const char *P6 = "tels_test_conc.tels";
    DeleteFileA(P6);
    c = tels_open(P6, TRUE);
    if (c) {
        TelsId big = 0;
        tels_create(c, TELS_ROOT, "big.bin", TELS_TYPE_FILE, &big);
        TelsWriterArg wa[4];
        HANDLE th[4];
        for (int t = 0; t < 4; t++) {
            wa[t].c = c;
            wa[t].id = big;
            wa[t].t = t;
            th[t] = CreateThread(NULL, 0, tels_writer_proc, &wa[t], 0, NULL);
        }
        WaitForMultipleObjects(4, th, TRUE, 30000);
        for (int t = 0; t < 4; t++) CloseHandle(th[t]);
        uint8_t blk[512];
        BOOL conc_ok = TRUE;
        for (int i = 0; i < 200 && conc_ok; i++) {
            for (int t = 0; t < 4 && conc_ok; t++) {
                uint64_t off = (uint64_t)(i * 4 + t) * 512u;
                if (tels_read(c, big, off, blk, 512) != 512) conc_ok = FALSE;
                for (int k = 0; k < 512 && conc_ok; k++)
                    if (blk[k] != (uint8_t)('A' + t)) conc_ok = FALSE;
            }
        }
        TEST("four concurrent writers land their ranges without corruption", conc_ok);
        TEST("commit after concurrent writes", tels_commit(c));
        tels_close(c);
    }

    /* یک پوشه با ۲۰۰۰ فایل: فهرست و بازیابی درست باشد */
    const char *P7 = "tels_test_many.tels";
    DeleteFileA(P7);
    c = tels_open(P7, TRUE);
    if (c) {
        TelsId many = 0;
        tels_create(c, TELS_ROOT, "many", TELS_TYPE_DIR, &many);
        BOOL made_all = TRUE;
        for (int i = 0; i < 2000 && made_all; i++) {
            char nm[24];
            snprintf(nm, sizeof(nm), "f%04d.dat", i);
            made_all = tels_create(c, many, nm, TELS_TYPE_FILE, NULL);
        }
        TEST("2000 files created in one directory", made_all);
        static TelsId ids[2100];
        TEST("list reports all 2000 files", tels_list(c, many, ids, 2100) == 2000);
        TEST("commit with 2000 entries", tels_commit(c));
        tels_close(c);
        c = tels_open(P7, FALSE);
        TelsId many2 = 0;
        TEST("reopen keeps all 2000 files",
             c && tels_lookup(c, TELS_ROOT, "many", &many2) && tels_list(c, many2, ids, 2100) == 2000);
        if (c) tels_close(c);
    }

    DeleteFileA(P);
    DeleteFileA(P2);
    DeleteFileA(P3);
    DeleteFileA(P4);
    DeleteFileA(P5);
    DeleteFileA(P6);
    DeleteFileA(P7);
}

/* ======================================================
   تست: رمزنگاری کانتینر — CRC و GCM با هم، کلید DPAPI، و عدم نشت متن ساده
   ====================================================== */
static uint32_t tels_t_crc32(const uint8_t *p, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void tels_t_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t tels_t_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static BOOL tels_t_read(const char *path, uint64_t pos, void *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;
    BOOL ok = (fseek(f, (long)pos, SEEK_SET) == 0) && (fread(buf, 1, n, f) == n);
    fclose(f);
    return ok;
}

static BOOL tels_t_write(const char *path, uint64_t pos, const void *buf, size_t n) {
    FILE *f = fopen(path, "r+b");
    if (!f) return FALSE;
    BOOL ok = (fseek(f, (long)pos, SEEK_SET) == 0) && (fwrite(buf, 1, n, f) == n);
    fclose(f);
    return ok;
}

/* رکوردهای فایل را از آفست داده‌ها فهرست می‌کند: موقعیت و نوع هر کدام */
static int tels_t_scan(const char *path, uint64_t *pos_out, uint8_t *kind_out, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    int n = 0;
    uint64_t pos = 12288u;
    while (n < max && pos + 64u <= (uint64_t)fsize) {
        uint8_t hdr[64];
        if (!tels_t_read(path, pos, hdr, sizeof(hdr))) break;
        if (tels_t_get32(hdr) != 0x4345524Cu) break;
        uint32_t plen = tels_t_get32(hdr + 20);
        if (pos + 64u + plen > (uint64_t)fsize) break;
        pos_out[n] = pos;
        kind_out[n] = hdr[8];
        n++;
        pos += 64u + plen;
    }
    return n;
}

void test_tels_encryption(void) {
    SECTION("Container encryption: GCM with CRC, DPAPI key protection, no plaintext leaks");

    /* ۱) متن ساده در فایل نیست و کانتینر با کلید DPAPI دوباره باز می‌شود */
    const char *PS = "tels_test_secret.tels";
    DeleteFileA(PS);
    TelsContainer *c = tels_open(PS, TRUE);
    TEST("encrypted container created", c != NULL);
    if (c) {
        TelsId f = 0;
        TEST("entry with a secret name created", tels_create(c, TELS_ROOT, "secretname_xyz.txt", TELS_TYPE_FILE, &f));
        TEST("secret payload written", tels_write(c, f, 0, "SECRET-PAYLOAD-7F3A9C", 21));
        TEST("commit", tels_commit(c));
        tels_close(c);
    }
    static uint8_t raw[1 << 16];
    FILE *fp = fopen(PS, "rb");
    size_t rn = fp ? fread(raw, 1, sizeof(raw), fp) : 0;
    if (fp) fclose(fp);
    TEST("file bytes do not contain the payload", rn > 0 && !bytes_contain((const char *)raw, rn, "SECRET-PAYLOAD"));
    TEST("file bytes do not contain the entry name", rn > 0 && !bytes_contain((const char *)raw, rn, "secretname_xyz"));
    c = tels_open(PS, FALSE);
    TEST("encrypted container reopens with its protected key", c != NULL);
    if (c) {
        TelsId f2 = 0;
        char pb[32] = {0};
        TEST("verify passes on a clean encrypted container", tels_verify(c));
        TEST("payload decrypts after reopen",
             tels_lookup(c, TELS_ROOT, "secretname_xyz.txt", &f2) &&
             tels_read(c, f2, 0, pb, 21) == 21 && memcmp(pb, "SECRET-PAYLOAD-7F3A9C", 21) == 0);
        tels_close(c);
    }

    /* ۲) nonce هر رکورد یکتاست: دو رکورد با متن یکسان، رمز متفاوت دارند */
    const char *PN = "tels_test_nonce.tels";
    DeleteFileA(PN);
    c = tels_open(PN, TRUE);
    if (c) {
        TelsId f = 0;
        tels_create(c, TELS_ROOT, "same.bin", TELS_TYPE_FILE, &f);
        tels_write(c, f, 0, "SAMEPAYLOAD", 11);
        tels_write(c, f, 11, "SAMEPAYLOAD", 11);
        tels_commit(c);
        tels_close(c);
    }
    uint64_t rp[8];
    uint8_t rk[8];
    int rcount = tels_t_scan(PN, rp, rk, 8);
    uint64_t dpos[2];
    int dcount = 0;
    for (int i = 0; i < rcount && dcount < 2; i++)
        if (rk[i] == 1) dpos[dcount++] = rp[i];
    TEST("two data records found", dcount == 2);
    if (dcount == 2) {
        uint8_t h1[64], h2[64], c1[11], c2[11];
        tels_t_read(PN, dpos[0], h1, 64);
        tels_t_read(PN, dpos[0] + 64, c1, 11);
        tels_t_read(PN, dpos[1], h2, 64);
        tels_t_read(PN, dpos[1] + 64, c2, 11);
        TEST("nonces of identical payloads differ", memcmp(h1 + 32, h2 + 32, 12) != 0);
        TEST("ciphertexts of identical payloads differ", memcmp(c1, c2, 11) != 0);
    }

    /* ۳) دست‌کاری با CRC معتبر: GCM باید رد کند و داده‌ی دست‌کاری‌شده برنگردد */
    const char *PT = "tels_test_tamper.tels";
    DeleteFileA(PT);
    c = tels_open(PT, TRUE);
    if (c) {
        TelsId t = 0;
        tels_create(c, TELS_ROOT, "t.bin", TELS_TYPE_FILE, &t);
        tels_write(c, t, 0, "ABCDEFGH", 8);
        tels_commit(c);
        tels_close(c);
    }
    uint64_t tp[8];
    uint8_t tk[8];
    int tcount = tels_t_scan(PT, tp, tk, 8);
    int tdata = -1;
    for (int i = 0; i < tcount; i++)
        if (tk[i] == 1) { tdata = i; break; }
    TEST("tamper target record found", tdata >= 0);
    if (tdata >= 0) {
        uint64_t rec = tp[tdata];
        uint8_t hdr[64], body[8];
        tels_t_read(PT, rec, hdr, sizeof(hdr));
        tels_t_read(PT, rec + 64, body, sizeof(body));
        body[2] ^= 0x01;
        uint32_t crc = tels_t_crc32(hdr, 60, 0);
        crc = tels_t_crc32(body, sizeof(body), crc);
        tels_t_put32(hdr + 60, crc);
        tels_t_write(PT, rec, hdr, sizeof(hdr));
        tels_t_write(PT, rec + 64, body, sizeof(body));
    }
    c = tels_open(PT, FALSE);
    TEST("container opens; the tampered record is rejected", c != NULL);
    if (c) {
        TelsId tf = 0;
        TEST("file created before the tampered record survives", tels_lookup(c, TELS_ROOT, "t.bin", &tf));
        char vb[16] = {0};
        TEST("reading the tampered data fails authentication", tels_read(c, tf, 0, vb, 8) == -1);
        TEST("tels_verify reports the tampering", !tels_verify(c));
        tels_close(c);
    }

    /* ۳ب) رکوردی که بعد از آخرین commit است و دست‌کاری شده: replay باید GCM را بررسی کند */
    const char *PU = "tels_test_replay.tels";
    DeleteFileA(PU);
    c = tels_open(PU, TRUE);
    if (c) {
        TelsId u = 0;
        tels_create(c, TELS_ROOT, "u.bin", TELS_TYPE_FILE, &u);
        tels_commit(c);
        tels_write(c, u, 0, "IJKLMNOP", 8);
        tels_abort(c);
    }
    uint64_t up[8];
    uint8_t uk[8];
    int ucount = tels_t_scan(PU, up, uk, 8);
    int udata = -1;
    for (int i = 0; i < ucount; i++)
        if (uk[i] == 1) { udata = i; break; }
    TEST("replay target record found", udata >= 0);
    if (udata >= 0) {
        uint64_t rec = up[udata];
        uint8_t hdr[64], body[8];
        tels_t_read(PU, rec, hdr, sizeof(hdr));
        tels_t_read(PU, rec + 64, body, sizeof(body));
        body[5] ^= 0x10;
        uint32_t crc = tels_t_crc32(hdr, 60, 0);
        crc = tels_t_crc32(body, sizeof(body), crc);
        tels_t_put32(hdr + 60, crc);
        tels_t_write(PU, rec, hdr, sizeof(hdr));
        tels_t_write(PU, rec + 64, body, sizeof(body));
    }
    c = tels_open(PU, FALSE);
    TEST("replay stops at a record that fails authentication", c != NULL);
    if (c) {
        TelsId u2 = 0;
        uint64_t usz = 99;
        TEST("file survives and its forged data is dropped on replay",
             tels_lookup(c, TELS_ROOT, "u.bin", &u2) && tels_size(c, u2, &usz) && usz == 0);
        tels_close(c);
    }
    DeleteFileA(PU);

    /* ۴) کلید wrap شده دست‌کاری شود: DPAPI باید باز کردن را رد کند */
    const char *PK = "tels_test_keytamper.tels";
    DeleteFileA(PK);
    c = tels_open(PK, TRUE);
    if (c) tels_close(c);
    {
        uint8_t hdr[64];
        tels_t_read(PK, 8192u, hdr, sizeof(hdr));
        uint32_t blen = tels_t_get32(hdr + 20);
        uint8_t blob[1024];
        if (blen > 0 && blen <= sizeof(blob)) {
            tels_t_read(PK, 8192u + 64u, blob, blen);
            blob[20] ^= 0x01;
            uint32_t crc = tels_t_crc32(hdr, 60, 0);
            crc = tels_t_crc32(blob, blen, crc);
            tels_t_put32(hdr + 60, crc);
            tels_t_write(PK, 8192u, hdr, sizeof(hdr));
            tels_t_write(PK, 8192u + 64u, blob, blen);
        }
    }
    c = tels_open(PK, FALSE);
    TEST("a tampered key blob is rejected by DPAPI", c == NULL);
    if (c) tels_close(c);

    /* ۵) فایلی که کانتینر نیست رد شود */
    const char *PG = "tels_test_garbage.tels";
    {
        uint8_t junk[20000];
        memset(junk, 0xAB, sizeof(junk));
        FILE *g = fopen(PG, "wb");
        if (g) { fwrite(junk, 1, sizeof(junk), g); fclose(g); }
    }
    TEST("a file that is not a container is rejected", tels_open(PG, FALSE) == NULL);

    DeleteFileA(PS);
    DeleteFileA(PN);
    DeleteFileA(PT);
    DeleteFileA(PK);
    DeleteFileA(PG);
}

/* ======================================================
   تست: لبه‌های API کانتینر — بافر نام و اندازه‌ی خواندن
   ====================================================== */
void test_tels_api_edges(void) {
    SECTION("Container API edges: name buffers and read sizes");
    const char *P = "tels_test_edges.tels";
    DeleteFileA(P);
    TelsContainer *c = tels_open(P, TRUE);
    TEST("edges container created", c != NULL);
    if (!c) return;

    TelsId id = 0;
    tels_create(c, TELS_ROOT, "long_name_example.txt", TELS_TYPE_FILE, &id);

    char small[8];
    memset(small, 'Z', sizeof(small));
    TelsType t = (TelsType)0;
    TelsId par = 0;
    TEST("tels_info refuses a name that does not fit", !tels_info(c, id, &t, &par, small, sizeof(small)));
    TEST("tels_info leaves a too-small buffer untouched", small[0] == 'Z' && small[7] == 'Z');

    char full[64];
    memset(full, 0, sizeof(full));
    TEST("tels_info copies a name that fits",
         tels_info(c, id, &t, &par, full, sizeof(full)) && strcmp(full, "long_name_example.txt") == 0);

    tels_write(c, id, 0, "abc", 3);
    char rb[8];
    memset(rb, 0, sizeof(rb));
    TEST("tels_read of zero bytes returns 0", tels_read(c, id, 0, rb, 0) == 0);
    TEST("tels_read past the end returns 0", tels_read(c, id, 100, rb, 4) == 0);
    TEST("tels_read returns only the bytes available",
         tels_read(c, id, 0, rb, 8) == 3 && memcmp(rb, "abc", 3) == 0);

    tels_abort(c);
    DeleteFileA(P);
}

/* ======================================================
   تست: فروشگاه lass — نسل‌ها، مقدار بزرگ، و ماندگاری
   ====================================================== */
void test_lass_store(void) {
    SECTION("Lass store (generations, large values, durability)");
    const char *SP = "tels_test_store.tels";
    lass_store_close();
    DeleteFileA(SP);
    TEST("store opens a new file", lass_store_open(SP));
    TEST("missing value does not exist", !lass_store_exists("a/b", "k"));
    TEST("put creates a value", lass_store_put("a/b", "k", "first", 5));
    TEST("value exists after put", lass_store_exists("a/b", "k"));

    uint8_t *got = NULL;
    uint32_t glen = 0;
    TEST("get returns the value",
         lass_store_get("a/b", "k", &got, &glen) && glen == 5 && memcmp(got, "first", 5) == 0);
    free(got);
    got = NULL;

    TEST("second put replaces the value", lass_store_put("a/b", "k", "second!", 7));
    TEST("get returns the newest generation",
         lass_store_get("a/b", "k", &got, &glen) && glen == 7 && memcmp(got, "second!", 7) == 0);
    free(got);
    got = NULL;

    static uint8_t big[9u * 1024u * 1024u];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 13u + 7u);
    TEST("large value (spans several chunks) is stored",
         lass_store_put("big", "blob", big, (uint32_t)sizeof(big)));
    TEST("large value reads back exactly",
         lass_store_get("big", "blob", &got, &glen) && glen == sizeof(big) && memcmp(got, big, sizeof(big)) == 0);
    free(got);
    got = NULL;

    TEST("delete removes the value", lass_store_delete("a/b", "k") && !lass_store_exists("a/b", "k"));
    TEST("empty value is stored", lass_store_put("a/b", "empty", NULL, 0));
    TEST("empty value reads back as length 0",
         lass_store_get("a/b", "empty", &got, &glen) && glen == 0);
    free(got);
    got = NULL;

    TEST("durable value written", lass_store_put("a/b", "keep", "durable", 7));
    lass_store_close();
    TEST("store reopens", lass_store_open(SP));
    TEST("committed value survives reopen",
         lass_store_get("a/b", "keep", &got, &glen) && glen == 7 && memcmp(got, "durable", 7) == 0);
    free(got);
    lass_store_close();
    DeleteFileA(SP);
}

/* ======================================================
   تست: مهاجرت پوشه‌ی قدیمی state ها به فروشگاه (یک‌بار، بدون بازنویسی)
   ====================================================== */
void test_state_migration(void) {
    SECTION("Smart Resume: legacy state folder migrates into the store");
    lass_store_close();
    DeleteFileA("tels_test_mig_store.tels");
    TEST("migration store opens", lass_store_open("tels_test_mig_store.tels"));

    const char *DIR = "tels_test_legacy_states";
    const char *legacy_file = "tels_test_legacy_states\\sr_00112233aabbccdd.dat";
    CreateDirectoryA(DIR, NULL);
    uint8_t first[96];
    for (size_t i = 0; i < sizeof(first); i++) first[i] = (uint8_t)(i * 7u + 1u);
    FILE *f = fopen(legacy_file, "wb");
    if (f) { fwrite(first, 1, sizeof(first), f); fclose(f); }

    uint8_t k8[8] = {0x5A, 0x13, 0x77, 0x21, 0x09, 0xC4, 0x3E, 0x88};
    IDM_PluginHooks *plug = smart_resume_plugin_create(DIR, k8);
    TEST("plugin created over a legacy folder", plug != NULL);

    uint8_t *got = NULL;
    uint32_t glen = 0;
    TEST("legacy state imported into the store",
         lass_store_get("states", "sr_00112233aabbccdd", &got, &glen) &&
         glen == sizeof(first) && memcmp(got, first, sizeof(first)) == 0);
    free(got);
    got = NULL;

    /* بار دوم: مقدار موجود در فروشگاه نباید با فایل قدیمی بازنویسی شود */
    uint8_t changed[96];
    memset(changed, 0xEE, sizeof(changed));
    f = fopen(legacy_file, "wb");
    if (f) { fwrite(changed, 1, sizeof(changed), f); fclose(f); }
    if (plug) smart_resume_plugin_destroy(plug);
    plug = smart_resume_plugin_create(DIR, k8);
    TEST("second import keeps the stored value",
         lass_store_get("states", "sr_00112233aabbccdd", &got, &glen) &&
         glen == sizeof(first) && memcmp(got, first, sizeof(first)) == 0);
    free(got);
    if (plug) smart_resume_plugin_destroy(plug);

    DeleteFileA(legacy_file);
    RemoveDirectoryA(DIR);
    lass_store_close();
    DeleteFileA("tels_test_mig_store.tels");
}

/* ======================================================
   تست: امنیت — DPAPI encryption برای proxy credentials
   ====================================================== */
void test_secure_store(void) {
    SECTION("Security — DPAPI Credential Encryption");

    char enc[1024] = {0};
    char dec[512]  = {0};

    /* رشته‌ی خالی — نباید crash کنه */
    TEST("encrypt empty string safe",
        secure_store_encrypt("", enc, sizeof(enc)));
    TEST("decrypt empty string safe",
        secure_store_decrypt("", dec, sizeof(dec)));

    /* رمزنگاری یه proxy URL واقعی با credential */
    const char *secret = "http://myuser:mypassword123@proxy.example.com:8080";
    BOOL enc_ok = secure_store_encrypt(secret, enc, sizeof(enc));
    TEST("encrypt proxy url succeeds", enc_ok);

    if (enc_ok) {
        TEST("encrypted output has DPAPI prefix",
            strncmp(enc, "DPAPI:", 6) == 0);
        TEST("encrypted output does not contain plaintext password",
            strstr(enc, "mypassword123") == NULL);

        /* رمزگشایی باید همون مقدار اصلی رو برگردونه */
        BOOL dec_ok = secure_store_decrypt(enc, dec, sizeof(dec));
        TEST("decrypt succeeds", dec_ok);
        TEST("decrypted value matches original",
            dec_ok && strcmp(dec, secret) == 0);
    }

    /* سازگاری با نسخه‌ی قدیمی: plaintext بدون پیشوند DPAPI باید
       بدون تغییر برگرده (upgrade path) */
    char legacy_dec[256] = {0};
    BOOL legacy_ok = secure_store_decrypt("http://old-plaintext-proxy.com",
                                          legacy_dec, sizeof(legacy_dec));
    TEST("legacy plaintext passthrough works", legacy_ok);
    TEST("legacy value unchanged",
        strcmp(legacy_dec, "http://old-plaintext-proxy.com") == 0);

    /* null safety */
    TEST("encrypt null plaintext safe",  !secure_store_encrypt(NULL, enc, sizeof(enc)));
    TEST("encrypt null out safe",        !secure_store_encrypt("x", NULL, 10));
    TEST("decrypt null in safe",         !secure_store_decrypt(NULL, dec, sizeof(dec)));
    TEST("decrypt null out safe",        !secure_store_decrypt("x", NULL, 10));
}

/* ======================================================
   main
   ====================================================== */
/* ======================================================
   تست: compaction در سطح container — داده‌ی زنده، ساختار درخت، بدون متن ساده
   ====================================================== */
static uint64_t test_file_bytes(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | (uint64_t)a.nFileSizeLow;
}

BOOL test_read_file(const char *path, uint8_t **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return FALSE;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    BOOL ok = (buf != NULL) && (fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    if (!ok) {
        free(buf);
        return FALSE;
    }
    *out = buf;
    *len = (size_t)n;
    return TRUE;
}

static BOOL test_bytes_contain(const uint8_t *hay, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || hn < nn) return FALSE;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0) return TRUE;
    }
    return FALSE;
}

void test_tels_compaction(void) {
    SECTION("Container compaction: live data and tree kept, no plaintext in the new file");
    const char *SRC   = "tels_test_compact_src.tels";
    const char *DST   = "tels_test_compact_dst.tels";
    const char *BUSY  = "tels_test_compact_busy.tels";
    const char *SMALL = "tels_test_compact_small.tels";
    DeleteFileA(SRC);
    DeleteFileA(DST);
    DeleteFileA(BUSY);
    DeleteFileA(SMALL);

    TelsContainer *tiny = tels_open(SMALL, TRUE);
    TEST("small container created", tiny != NULL);
    if (tiny) {
        TelsId sa = 0;
        tels_create(tiny, TELS_ROOT, "a.txt", TELS_TYPE_FILE, &sa);
        tels_write(tiny, sa, 0, "hello", 5);
        tels_commit(tiny);
        TEST("a small container does not need compaction", !tels_should_compact(tiny));
        tels_close(tiny);
    }
    DeleteFileA(SMALL);

    TelsContainer *c = tels_open(SRC, TRUE);
    TEST("compaction source created", c != NULL);
    if (!c) return;

    /* ۳۰۰ بار نوشتن و commit روی یک فایل: نسل‌های قدیمی فقط داخل فایل مانده‌اند */
    static uint8_t block[8192];
    static uint8_t expect[8192];
    TelsId churn = 0;
    tels_create(c, TELS_ROOT, "churn.dat", TELS_TYPE_FILE, &churn);
    for (uint32_t round = 0; round < 300; round++) {
        memset(block, (int)(round & 0xFFu), sizeof(block));
        memcpy(block, "COMPACT-MARKER", 14);
        tels_write(c, churn, 0, block, sizeof(block));
        tels_commit(c);
    }
    memcpy(expect, block, sizeof(block));
    TEST("garbage from 300 replacements is detected", tels_should_compact(c));

    TelsId inner = 0, note = 0, sparse = 0, empty = 0, gone = 0;
    TEST("nested folder created", tels_resolve(c, "docs/inner", TRUE, &inner));
    tels_create(c, inner, "note.txt", TELS_TYPE_FILE, &note);
    tels_write(c, note, 0, "nested note", 11);
    tels_create(c, TELS_ROOT, "sparse.bin", TELS_TYPE_FILE, &sparse);
    tels_write(c, sparse, 100000, "tail-data", 9);
    tels_create(c, TELS_ROOT, "empty.bin", TELS_TYPE_FILE, &empty);
    tels_create(c, TELS_ROOT, "gone.txt", TELS_TYPE_FILE, &gone);
    tels_write(c, gone, 0, "bye", 3);
    tels_delete(c, gone);
    tels_commit(c);

    TEST("compaction writes a new container", tels_compact_to(c, DST));
    uint64_t src_bytes = test_file_bytes(SRC);
    tels_close(c);
    uint64_t dst_bytes = test_file_bytes(DST);
    TEST("compacted file is far smaller than the source",
         src_bytes > 0 && dst_bytes > 0 && dst_bytes * 10u < src_bytes);

    TelsContainer *d = tels_open(DST, FALSE);
    TEST("compacted file opens", d != NULL);
    if (!d) {
        DeleteFileA(SRC);
        DeleteFileA(DST);
        return;
    }
    TEST("compacted file verifies", tels_verify(d));

    uint8_t got[8192];
    TelsId cid = 0;
    uint64_t csz = 0;
    TEST("churn.dat keeps only its newest content",
         tels_resolve(d, "churn.dat", FALSE, &cid) && tels_size(d, cid, &csz) && csz == sizeof(got) &&
         tels_read(d, cid, 0, got, sizeof(got)) == (int32_t)sizeof(got) &&
         memcmp(got, expect, sizeof(got)) == 0);

    char nb[16];
    memset(nb, 0, sizeof(nb));
    TelsId nid = 0;
    TEST("nested folder keeps its file",
         tels_resolve(d, "docs/inner/note.txt", FALSE, &nid) &&
         tels_read(d, nid, 0, nb, 11) == 11 && memcmp(nb, "nested note", 11) == 0);

    TelsId sid = 0;
    uint64_t ssz = 0;
    TEST("sparse file keeps its size",
         tels_resolve(d, "sparse.bin", FALSE, &sid) && tels_size(d, sid, &ssz) && ssz == 100009u);
    TEST("sparse file keeps its hole and its tail",
         tels_read(d, sid, 0, nb, 16) == 16 && nb[0] == 0 && nb[15] == 0 &&
         tels_read(d, sid, 100000, nb, 9) == 9 && memcmp(nb, "tail-data", 9) == 0);

    TelsId eid = 0;
    uint64_t esz = 1;
    TEST("empty file survives with size 0",
         tels_resolve(d, "empty.bin", FALSE, &eid) && tels_size(d, eid, &esz) && esz == 0);
    TelsId xid = 0;
    TEST("deleted file is not copied", !tels_resolve(d, "gone.txt", FALSE, &xid));

    uint8_t *raw = NULL;
    size_t rawn = 0;
    BOOL raw_ok = test_read_file(DST, &raw, &rawn);
    TEST("compacted file can be read as raw bytes", raw_ok);
    TEST("compacted file holds no plaintext payload",
         raw_ok && !test_bytes_contain(raw, rawn, "COMPACT-MARKER"));
    free(raw);

    TelsId after = 0;
    TEST("writes work in the compacted file",
         tels_create(d, TELS_ROOT, "after.txt", TELS_TYPE_FILE, &after) &&
         tels_write(d, after, 0, "ok", 2) && tels_commit(d));
    tels_close(d);

    TelsContainer *r = tels_open(DST, FALSE);
    TelsId aid = 0;
    char ab[2] = {0, 0};
    TEST("compacted file reopens with the new write",
         r != NULL && tels_resolve(r, "after.txt", FALSE, &aid) &&
         tels_read(r, aid, 0, ab, 2) == 2 && ab[0] == 'o' && ab[1] == 'k');
    if (r) tels_close(r);

    FILE *bf = fopen(BUSY, "wb");
    if (bf) {
        fputs("not a container", bf);
        fclose(bf);
    }
    TelsContainer *s2 = tels_open(DST, FALSE);
    TEST("compaction refuses an existing destination", s2 != NULL && !tels_compact_to(s2, BUSY));
    TEST("the existing destination is left alone", test_file_bytes(BUSY) == strlen("not a container"));
    TEST("the source is unchanged after a refused compaction", s2 != NULL && tels_verify(s2));
    if (s2) tels_close(s2);

    DeleteFileA(SRC);
    DeleteFileA(DST);
    DeleteFileA(BUSY);
}

/* ======================================================
   تست: compaction فروشگاه — خودکار، بدون از دست دادن مقدار، حذف فایل موقت کرش‌کرده
   ====================================================== */
void test_store_compaction(void) {
    SECTION("Store compaction: space reclaimed while writing, every value kept, stale temp removed");
    const char *SP  = "tels_test_compact_store.tels";
    const char *SPT = "tels_test_compact_store.tels.compact";
    lass_store_close();
    DeleteFileA(SP);
    DeleteFileA(SPT);

    FILE *leftover = fopen(SPT, "wb");
    if (leftover) {
        fputs("leftover from an interrupted compaction", leftover);
        fclose(leftover);
    }
    TEST("store opens next to a stale temp file", lass_store_open(SP));
    TEST("stale temp file is removed on open", GetFileAttributesA(SPT) == INVALID_FILE_ATTRIBUTES);
    TEST("value written before the churn", lass_store_put("dl", "keep", "stable-value", 12));

    static uint8_t val[8192];
    BOOL all_ok = TRUE;
    for (uint32_t i = 0; i < 300 && all_ok; i++) {
        memset(val, (int)(i & 0xFFu), sizeof(val));
        memcpy(val, &i, sizeof(i));
        all_ok = lass_store_put("q", "state", val, (uint32_t)sizeof(val));
    }
    TEST("300 replacements of an 8 KB value all succeed", all_ok);
    TEST("store file stays far below the 2.4 MB written", test_file_bytes(SP) < 1024u * 1024u);

    uint8_t *got = NULL;
    uint32_t glen = 0;
    TEST("newest state survives the compactions",
         lass_store_get("q", "state", &got, &glen) && glen == sizeof(val) &&
         memcmp(got, val, sizeof(val)) == 0);
    free(got);
    got = NULL;
    TEST("value written before the churn survives",
         lass_store_get("dl", "keep", &got, &glen) && glen == 12 && memcmp(got, "stable-value", 12) == 0);
    free(got);
    got = NULL;

    lass_store_close();
    TEST("store reopens after compactions", lass_store_open(SP));
    TEST("newest state is still there after reopen",
         lass_store_get("q", "state", &got, &glen) && glen == sizeof(val) &&
         memcmp(got, val, sizeof(val)) == 0);
    free(got);
    lass_store_close();
    TEST("no temp file is left behind", GetFileAttributesA(SPT) == INVALID_FILE_ATTRIBUTES);
    DeleteFileA(SP);
    DeleteFileA(SPT);
}

