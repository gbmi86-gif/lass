/* lass_store.h — فروشگاه داده‌ی برنامه روی container .tels

   صف دانلودها و state های ادامه‌ی دانلود داخل همین container نگه داشته می‌شوند.
   هر مقدار با یک پوشه و یک نام ذخیره می‌شود. جایگزینی اتمیک است: نسل جدید نوشته
   و کامیت می‌شود، بعد نسل‌های قدیمی حذف می‌شوند؛ پس داده‌ی کامل حتی با کرش وسط
   کار از دست نمی‌رود. */
#ifndef LASS_STORE_H
#define LASS_STORE_H

#include <windows.h>
#include <stdint.h>

/* باز یا ایجاد می‌کند؛ اگر قبلاً باز بود اول می‌بندد. فایل خراب را دست نمی‌زند. */
BOOL lass_store_open(const char *path);

/* %APPDATA%\Lass\lass.tels */
BOOL lass_store_open_default(void);

/* commit و بستن؛ چند بار صدا زدنش بی‌خطر است */
void lass_store_close(void);

/* مقدار را جایگزین می‌کند؛ اگر len صفر است data می‌تواند NULL باشد */
BOOL lass_store_put(const char *dir, const char *name, const void *data, uint32_t len);

/* کپی تازه‌ی بزرگ‌ترین نسل را برمی‌گرداند؛ *out را caller با free آزاد می‌کند */
BOOL lass_store_get(const char *dir, const char *name, uint8_t **out, uint32_t *len);

BOOL lass_store_exists(const char *dir, const char *name);

/* همه‌ی نسل‌های یک مقدار را حذف می‌کند */
BOOL lass_store_delete(const char *dir, const char *name);

#endif /* LASS_STORE_H */
