/* test_media_store.c — کانتینر، نام فایل و اثر انگشت مشترک (media_store) */
#include "test_common.h"
#include "media_store.h"

#define MS_TELS    "mstore_test.tels"
#define MS_DIR     "mstore_dir"
#define MS_FILE_A  "mstore_dir\\movie.ts"
#define MS_BASIS   1469598103934665603ULL

static const char *const MS_HLS_STRIP[]  = { ".m3u8", ".m3u", ".ts", ".mp4", ".m4s", ".m4v" };
static const char *const MS_DASH_STRIP[] = { ".mpd", ".mp4", ".m4s", ".m4v", ".ts" };

static BOOL ms_make_file(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    return TRUE;
}

void test_media_store(void) {
    uint8_t data[10000], got[10000], copybuf[4096];
    MediaHttp h;
    MediaBuf mb;
    TelsContainer *c, *c2;
    uint64_t sz = 0;
    char out[64], path[IDM_MAX_PATH], keep[4] = "ZZZ";
    HANDLE hf;
    DWORD rd = 0;
    uint32_t i;
    size_t n_hls = sizeof(MS_HLS_STRIP) / sizeof(MS_HLS_STRIP[0]);
    size_t n_dash = sizeof(MS_DASH_STRIP) / sizeof(MS_DASH_STRIP[0]);

    SECTION("Shared container, file names and fingerprint (media_store)");

    for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 31u + 7u);
    DeleteFileA(MS_TELS);
    memset(&h, 0, sizeof(h));
    c = media_container_open(MS_TELS, 1, &h);
    TEST("the container is created when it does not exist", c != NULL);
    if (c) {
        TEST("a value written in chunks is stored", media_entry_put(c, "data", data, sizeof(data), 4096));
        TEST("the entry exists after the put", media_entry_exists(c, "data"));
        TEST("the stored size is the written size", media_entry_size(c, "data", &sz) && sz == sizeof(data));
        memset(&mb, 0, sizeof(mb));
        TEST("the stored value reads back exactly",
             media_entry_read(c, "data", &mb, sizeof(data)) && mb.len == sizeof(data) &&
             memcmp(mb.data, data, sizeof(data)) == 0);
        media_buf_free(&mb);
        memset(&mb, 0, sizeof(mb));
        TEST("a value over the read limit is refused", !media_entry_read(c, "data", &mb, 100));
        media_buf_free(&mb);
        TEST("a second put replaces the value",
             media_entry_put(c, "data", (const uint8_t *)"hello", 5, 4096) &&
             media_entry_size(c, "data", &sz) && sz == 5);
        memset(&mb, 0, sizeof(mb));
        TEST("the replaced value is the new one",
             media_entry_read(c, "data", &mb, 100) && mb.len == 5 && memcmp(mb.data, "hello", 5) == 0);
        media_buf_free(&mb);
        TEST("a marker is an empty entry that exists",
             media_entry_marker(c, "mk") && media_entry_exists(c, "mk") &&
             media_entry_size(c, "mk", &sz) && sz == 0);
        TEST("dropping an entry removes it", media_entry_drop(c, "data") && !media_entry_exists(c, "data"));
        TEST("dropping a missing entry succeeds", media_entry_drop(c, "data"));
        TEST("a payload is stored for the copy test", media_entry_put(c, "payload", data, sizeof(data), 4096));
        hf = CreateFileA("mstore_copy.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            BOOL copied = media_entry_copy_to_handle(c, "payload", hf, copybuf, sizeof(copybuf));
            CloseHandle(hf);
            TEST("an entry is copied to a file in chunks", copied);
            hf = CreateFileA("mstore_copy.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                BOOL same = ReadFile(hf, got, sizeof(got), &rd, NULL) && rd == sizeof(data) &&
                            memcmp(got, data, sizeof(data)) == 0;
                CloseHandle(hf);
                TEST("the copied file holds the same bytes", same);
            }
        }
        DeleteFileA("mstore_copy.bin");
        TEST("clearing the container removes every entry",
             media_container_clear(c) && !media_entry_exists(c, "mk") && !media_entry_exists(c, "payload"));
        TEST("a value written before finishing", media_entry_put(c, "kept", (const uint8_t *)"xyz", 3, 4096));
        media_container_finish(c);
        c2 = media_container_open(MS_TELS, 1, &h);
        TEST("the container reopens after finishing", c2 != NULL);
        memset(&mb, 0, sizeof(mb));
        TEST("the value survives the reopen",
             c2 && media_entry_read(c2, "kept", &mb, 16) && mb.len == 3 && memcmp(mb.data, "xyz", 3) == 0);
        media_buf_free(&mb);
        media_container_finish(c2);
    }
    DeleteFileA(MS_TELS);

    TEST("a candidate name without a number", media_build_candidate(out, sizeof(out), "video", 0, ".ts") &&
         strcmp(out, "video.ts") == 0);
    TEST("a candidate name with a number", media_build_candidate(out, sizeof(out), "video", 2, ".ts") &&
         strcmp(out, "video (2).ts") == 0);
    TEST("a candidate that does not fit is refused", !media_build_candidate(out, 8, "video", 2, ".ts"));
    TEST("a missing file is reported as missing", !media_path_exists("mstore_no_such_file.bin"));
    CreateDirectoryA(MS_DIR, NULL);
    DeleteFileA(MS_FILE_A);
    TEST("an existing file is reported as present", ms_make_file(MS_FILE_A) && media_path_exists(MS_FILE_A));

    media_choose_final_path(MS_DIR "\\movie.m3u8", ".ts", MS_HLS_STRIP, n_hls, path, sizeof(path));
    TEST("a busy name gets a number (movie (1).ts)", strcmp(path, "mstore_dir\\movie (1).ts") == 0);
    DeleteFileA(MS_FILE_A);
    media_choose_final_path(MS_DIR "\\movie.m3u8", ".ts", MS_HLS_STRIP, n_hls, path, sizeof(path));
    TEST("a free name is used as it is", strcmp(path, MS_FILE_A) == 0);
    media_choose_final_path(MS_DIR "\\clip.mp4", ".mp4", MS_HLS_STRIP, n_hls, path, sizeof(path));
    TEST("a known source extension is replaced", strcmp(path, "mstore_dir\\clip.mp4") == 0);
    media_choose_final_path(MS_DIR "\\show.mpd", ".mp4", MS_DASH_STRIP, n_dash, path, sizeof(path));
    TEST("the DASH manifest extension is replaced", strcmp(path, "mstore_dir\\show.mp4") == 0);
    media_choose_final_path(MS_DIR "\\data.bin", ".ts", MS_HLS_STRIP, n_hls, path, sizeof(path));
    TEST("an unknown extension stays in the name", strcmp(path, "mstore_dir\\data.bin.ts") == 0);
    DeleteFileA(MS_FILE_A);
    RemoveDirectoryA(MS_DIR);

    media_strip_query("http://a.b/c/d.ts?token=1&x=2", out, sizeof(out));
    TEST("the query string is removed before fingerprinting", strcmp(out, "http://a.b/c/d.ts") == 0);
    media_strip_query("http://a.b/c/plain.ts", out, sizeof(out));
    TEST("a URL without a query is unchanged", strcmp(out, "http://a.b/c/plain.ts") == 0);
    media_strip_query("abcdef", out, 4);
    TEST("a short output buffer keeps what fits", strcmp(out, "abc") == 0);
    media_strip_query("abc", keep, 0);
    TEST("a zero-size output buffer is not written", keep[0] == 'Z');
    TEST("the fingerprint hash keeps its value (saved states depend on it)",
         media_fnv_mix(MS_BASIS, "a", 1) == 4953267810257967366ULL);
    TEST("the fingerprint hash of two parts equals the hash of their concatenation",
         media_fnv_mix(MS_BASIS, "ab", 2) == media_fnv_mix(media_fnv_mix(MS_BASIS, "a", 1), "b", 1));
    TEST("different bytes give different fingerprints",
         media_fnv_mix(MS_BASIS, "a", 1) != media_fnv_mix(MS_BASIS, "b", 1));
}
