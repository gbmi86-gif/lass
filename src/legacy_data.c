/* legacy_data.c — رجوع کن به legacy_data.h */
#include <windows.h>
#include <string.h>
#include "legacy_data.h"
#include "lass_log.h"

/* کپی امن رشته به بافر ثابت (بدون format string، تا هشدار truncation ندهد) */
static int copy_bounded(char *dst, size_t dst_sz, const char *src) {
    size_t n = strlen(src);
    if (dst_sz == 0 || n >= dst_sz) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

/* out = dir\name؛ اگر جا نشود 0 برمی‌گرداند */
static int join_path(char *out, size_t out_sz, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > out_sz) return 0;
    memcpy(out, dir, dl);
    out[dl] = '\\';
    memcpy(out + dl + 1, name, nl + 1);
    return 1;
}

/* ساخت زنجیره‌ی پوشه‌ها، سطح به سطح */
static void ensure_dir_chain(const char *dir) {
    char buf[MAX_PATH];
    if (!copy_bounded(buf, sizeof(buf), dir)) return;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p;
            *p = '\0';
            CreateDirectoryA(buf, NULL);   /* اگر از قبل باشد، خطا مهم نیست */
            *p = c;
        }
    }
    CreateDirectoryA(buf, NULL);
}

static void ensure_parent_dir(const char *file_path) {
    char dir[MAX_PATH];
    if (!copy_bounded(dir, sizeof(dir), file_path)) return;
    char *slash = strrchr(dir, '\\');
    if (!slash) return;
    *slash = '\0';
    ensure_dir_chain(dir);
}

int legacy_copy_file_if_missing(const char *legacy_path, const char *current_path) {
    if (!legacy_path || !current_path || !legacy_path[0] || !current_path[0]) return 0;
    if (GetFileAttributesA(current_path) != INVALID_FILE_ATTRIBUTES) return 0;
    if (GetFileAttributesA(legacy_path) == INVALID_FILE_ATTRIBUTES) return 0;
    ensure_parent_dir(current_path);
    if (!CopyFileA(legacy_path, current_path, TRUE)) {
        LOGW(LOG_MOD_CORE, "legacy data migration failed: %s (err=%lu)",
             legacy_path, (unsigned long)GetLastError());
        return 0;
    }
    LOGI(LOG_MOD_CORE, "migrated legacy data: %s -> %s", legacy_path, current_path);
    return 1;
}

int legacy_copy_dat_dir_if_missing(const char *legacy_dir, const char *current_dir) {
    if (!legacy_dir || !current_dir || !legacy_dir[0] || !current_dir[0]) return 0;
    DWORD attr = GetFileAttributesA(legacy_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;

    char pattern[MAX_PATH];
    if (!join_path(pattern, sizeof(pattern), legacy_dir, "*.dat")) return 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int copied = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char src[MAX_PATH], dst[MAX_PATH];
        if (!join_path(src, sizeof(src), legacy_dir, fd.cFileName)) continue;
        if (!join_path(dst, sizeof(dst), current_dir, fd.cFileName)) continue;
        copied += legacy_copy_file_if_missing(src, dst);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return copied;
}
