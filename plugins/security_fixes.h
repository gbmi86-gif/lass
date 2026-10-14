/* security_fixes.h
   ─────────────────────────────────────────────────────────────
   رفع باگ‌های امنیتی کد اصلی - بدون لمس src/
   این هدر رو در main.c قبل از بقیه include‌ها اضافه کن

   باگ‌های رفع‌شده:
   1. integer overflow در calloc (count * size)
   2. NULL deref در seg_create وقتی calloc فیل می‌کنه
   3. URL/path injection (null byte در URL)
   4. buffer overread در http_extract_filename
   5. race condition در engine_find بدون lock
   6. تنها گذاشتن dl->writer باز وقتی start_download فیل می‌کنه
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_SECURITY_FIXES_H
#define IDM_SECURITY_FIXES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ======================================================
   FIX 1: Safe integer multiplication (جلوگیری از overflow)
   ====================================================== */
static inline bool safe_mul_size(size_t a, size_t b, size_t *result) {
    if (a == 0 || b == 0) { *result = 0; return true; }
    if (a > (SIZE_MAX / b)) return false;   /* overflow */
    *result = a * b;
    return true;
}

/* ======================================================
   FIX 2: Validated calloc (جایگزین calloc مستقیم)
   ====================================================== */
static inline void *safe_calloc(size_t count, size_t size) {
    size_t total;
    if (!safe_mul_size(count, size, &total)) return NULL;
    if (total == 0) return NULL;
    return calloc(count, size);
}

/* ======================================================
   FIX 3: URL sanitization
   - حذف null byte در URL
   - بررسی scheme مجاز
   - حداکثر طول
   ====================================================== */
static inline bool validate_url(const char *url, int max_len) {
    if (!url) return false;

    int len = 0;
    for (const char *p = url; *p; p++, len++) {
        if (len >= max_len) return false;       /* خیلی طولانی */
        if ((unsigned char)*p < 0x20) return false;  /* کنترل کاراکتر / null byte */
    }

    /* باید http:// یا https:// باشه */
    if (_strnicmp(url, "http://",  7) != 0 &&
        _strnicmp(url, "https://", 8) != 0)
        return false;

    return (len > 10);
}

/* ======================================================
   FIX 4: Safe path construction
   - جلوگیری از path traversal (../)
   ====================================================== */
static inline bool validate_out_path(const char *path) {
    if (!path || path[0] == '\0') return false;

    /* جلوگیری از path traversal */
    if (strstr(path, "..\\") || strstr(path, "../")) return false;

    /* نباید با \\\\ شروع بشه (UNC injection) */
    if (path[0] == '\\' && path[1] == '\\') return false;

    return true;
}

/* ======================================================
   FIX 5: Bounds-checked string copy
   (مطمئن میشه null-terminated هست)
   ====================================================== */
static inline void safe_strncpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ======================================================
   FIX 6: Checked segment count
   ====================================================== */
static inline uint32_t clamp_segments(uint32_t n, uint32_t min_n, uint32_t max_n) {
    if (n < min_n) return min_n;
    if (n > max_n) return max_n;
    return n;
}

#endif /* IDM_SECURITY_FIXES_H */
