/* url_secure.h — جداسازی رمز از URL و پوشاندن آن در لاگ‌ها
   قاعده: رمز هرگز در صف، لاگ یا URL ذخیره‌شده‌ی دانلود نمی‌ماند. فقط در حافظه
   (secret_url) برای اتصال استفاده می‌شود و در صف به‌صورت رمزشده با DPAPI است. */
#ifndef LASS_URL_SECURE_H
#define LASS_URL_SECURE_H

#include <windows.h>
#include <stddef.h>

/* "scheme://user:pass@host/..." را به URL پاک (scheme://user@host/...) و رمز جدا می‌کند.
   URL بدون رمز دست‌نخورده برمی‌گردد و pass_out خالی می‌ماند. FALSE در صورت خطای اندازه. */
BOOL url_split_credentials(const char *in, char *clean_out, size_t clean_sz,
                           char *pass_out, size_t pass_sz);
/* URL پاک را با رمز داده‌شده کامل می‌کند؛ اگر URL از قبل رمز داشته باشد FALSE. */
BOOL url_inject_password(const char *clean, const char *pass, char *out, size_t out_sz);
/* نسخه‌ی قابل لاگ: هر "user:pass@" در متن (حتی داخل JSON) به "user:***@" تبدیل می‌شود. */
void url_redact_for_log(const char *in, char *out, size_t out_sz);

#endif
