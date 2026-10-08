/* secure_store.h — رمزنگاری داده‌های حساس با Windows DPAPI
   ─────────────────────────────────────────────────────────
   مسئله: proxy_url می‌تونه credential داشته باشه
   (مثل http://user:pass@host:port) و قبلاً plaintext در
   INI و Registry ذخیره می‌شد — یعنی هر برنامه یا کاربر دیگه‌ای
   با دسترسی به همون فایل/رجیستری می‌تونست بخونتش.

   راه‌حل: Windows DPAPI (CryptProtectData/CryptUnprotectData)
   داده رو با کلیدی که از credential های Windows کاربر مشتق
   میشه رمزنگاری می‌کنه. فقط همون user account روی همون ماشین
   می‌تونه decrypt کنه — حتی اگه فایل کپی بشه به ماشین دیگه یا
   توسط user دیگه‌ای خونده بشه، قابل decrypt نیست.

   این یه built-in Windows API هست (از Windows 2000 به بعد)،
   نیازی به کتابخانه‌ی خارجی نداره — همون فلسفه‌ی no-external-deps
   پروژه حفظ میشه.
   ─────────────────────────────────────────────────────────
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* رمزنگاری یه رشته‌ی plaintext و برگردوندن base64 برای ذخیره در
   INI/Registry (که فقط ASCII قبول می‌کنن). خروجی همیشه با
   "DPAPI:" شروع میشه تا از plaintext قدیمی (سازگاری با نسخه‌های
   قبل) قابل تشخیص باشه.
   برمی‌گردونه: TRUE در صورت موفقیت. */
BOOL secure_store_encrypt(const char *plaintext, char *out_b64, int out_sz);

/* رمزگشایی — اگه ورودی با "DPAPI:" شروع نشه (یعنی از نسخه‌ی
   قدیمی‌تر plaintext مونده)، همون رو بدون تغییر برمی‌گردونه —
   این یعنی upgrade از نسخه‌های قدیمی بدون از دست دادن تنظیمات
   کار می‌کنه، و دفعه‌ی بعد که save بشه رمزنگاری میشه. */
BOOL secure_store_decrypt(const char *in_b64_or_plain, char *out_plain, int out_sz);
