/* tests/test_httpd.h
   ─────────────────────────────────────────────────────────────
   یه HTTP/1.1 server خیلی مینیمال، فقط برای loopback (127.0.0.1)،
   که در test_all.c استفاده میشه تا رفتار *واقعی* چند دانلود
   همزمان به یه host واحد رو تست کنیم — نه فقط شبیه‌سازی دستی
   منطق scheduler (که تست‌های قبلی انجام می‌دادن و دقیقاً به همین
   خاطر باگ concurrency واقعی v5.0.7 رو هیچ‌وقت نگرفتن، چون هیچ‌کدوم
   واقعاً http_discover/http_open_range رو صدا نمی‌زدن).

   این سرور:
     - هر مسیری (path) رو با یه بدنه‌ی ساختگی به اندازه‌ی ثابت جواب
       میده (HEAD و GET، شامل Range برای GET)
     - بدنه رو trickle (تکه‌تکه، با یه Sleep کوچیک بین تکه‌ها) می‌فرسته
       تا connection چند صدم ثانیه به‌جای چند میلی‌ثانیه باز بمونه —
       این فرصت رو میده که چند دانلود واقعاً هم‌زمان در حال دریافت
       باشن و بشه این هم‌زمانی رو اندازه گرفت
     - یه شمارنده‌ی peak concurrent GET نگه می‌داره که تست ازش
       برای اثبات واقعی «چند connection همزمان باز بودن» استفاده
       می‌کنه (نه فقط state داخلی engine که می‌تونه گمراه‌کننده باشه)

   فقط برای تست — هیچ ادعای production-grade (HTTP parsing کامل،
   keep-alive، chunked encoding و غیره) نداره؛ فقط باید برای
   کلاینت WinINet ما (که رفتارش رو دقیقاً از http.c می‌شناسیم)
   کافی باشه.
   ────────────────────────────────────────────────────────────── */
#ifndef LASS_TEST_HTTPD_H
#define LASS_TEST_HTTPD_H

#include <stdint.h>
#include <stdbool.h>

/* راه‌اندازی سرور روی 127.0.0.1:port.
   body_size:      اندازه‌ی بدنه‌ی ساختگی هر فایل (بایت) — باید کمتر
                    از 2MB باشه تا engine فقط ۱ segment انتخاب کنه
                    (ساده‌تر برای تست: نیازی به هماهنگی چند Range
                    همزمان روی یه فایل نیست).
   trickle_total_ms: کل زمانی که سرور طول می‌کشه بدنه رو کامل
                    بفرسته (با چند تکه‌ی مساوی) — این عمداً connection
                    رو باز نگه می‌داره.
   بازگشت: true اگه listen موفق بود. */
bool test_httpd_start(uint16_t port, uint32_t body_size, uint32_t trickle_total_ms);

/* توقف امن سرور و بستن همه‌ی thread ها — باید قبل از خروج از
   پراسس تست صدا زده بشه. */
void test_httpd_stop(void);

/* بیشترین تعداد GET connection ای که سرور در آنِ واحد دیده —
   معیار اصلی تست برای اثبات concurrency واقعی. */
int test_httpd_peak_concurrent_gets(void);

/* چند GET تا الان کامل جواب داده شده — برای دیباگ/سنجش پیشرفت. */
int test_httpd_completed_gets(void);

/* چند GET رنج‌دار الان با هدر If-Range صحیح (ETag همین سرور) اومده —
   اثبات اینکه کلاینت واقعاً از سرور می‌پرسه «هنوز همون فایله؟». */
int test_httpd_if_range_gets(void);

#endif /* LASS_TEST_HTTPD_H */
