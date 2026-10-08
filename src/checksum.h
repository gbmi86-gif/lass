/* checksum.h
   ─────────────────────────────────────────────────────────────
   محاسبه‌ی چک‌سام فایل (MD5 / SHA-1 / SHA-256) با CryptoAPI خودِ
   ویندوز (advapi32) — بدون هیچ کتابخانه‌ی جانبی. فایل به‌صورت
   جریانی (chunk به chunk) خونده می‌شه، پس حافظه‌ی مصرفی مستقل از
   حجم فایله (حتی برای فایل‌های چند گیگابایتی).
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_CHECKSUM_H
#define LASS_CHECKSUM_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHECKSUM_MD5    = 0,
    CHECKSUM_SHA1   = 1,
    CHECKSUM_SHA256 = 2,
} ChecksumAlgo;

/* حداکثر طول رشته‌ی hex خروجی (SHA-256 = 64 کاراکتر + NUL) */
#define CHECKSUM_HEX_MAXLEN 65

/* نام الگوریتم برای نمایش توی UI ("MD5", "SHA-1", "SHA-256") */
const char *checksum_algo_name(ChecksumAlgo algo);

/* محاسبه‌ی sync — روی همون thread صدازننده بلاک می‌کنه، پس برای
   فایل‌های بزرگ باید از یه thread جدا صدا زده بشه (نمونه‌ش توی
   guo.c، دیالوگ Verify Checksum).
   خروجی: TRUE اگه موفق بود (out_hex پر شده با رشته‌ی hex
   کوچیک‌حرف)، FALSE اگه فایل باز نشد یا CryptoAPI شکست خورد. */
BOOL checksum_compute_file(const char *path, ChecksumAlgo algo,
                            char out_hex[CHECKSUM_HEX_MAXLEN]);

#ifdef __cplusplus
}
#endif

#endif /* LASS_CHECKSUM_H */
