/* shutdown_brake.h — ترمز خروج ناگهانی (Graceful Shutdown Coordinator)
   ─────────────────────────────────────────────────────────────
   هدف: یه رجیستری مرکزی از همه‌ی thread/subsystem های فعال که:

   ۱. هر thread/subsystem که ساخته میشه، خودش رو register می‌کنه
      (اولین چیزی که start میشه، مثل scheduler اصلی engine).
   ۲. هر thread/subsystem وقتی کارش تمومه، خودش رو
      unregister می‌کنه.
   ۳. در لحظه‌ی خروج برنامه (WM_DESTROY یا سیگنال دیگه)، این
      ماژول:
      - جلوی start شدن subsystem جدید رو می‌گیره (draining mode)
      - صبر می‌کنه (با timeout جهانی) تا همه‌ی subsystem های
        فعال خودشون رو unregister کنن
      - به ترتیب LIFO (آخرین‌واردشده اولین‌خارج‌شده) اونهایی
        که هنوز زنده‌ن رو force-timeout می‌کنه و لاگ می‌کنه —
        این دقیقاً همون "از همه دیرتر خارج بشه" هست: اولین
        subsystem که register شده (معمولاً scheduler اصلی
        engine)، آخرین چیزیه که منتظرش می‌مونیم چون همه‌ی
        subsystem های دیگه بهش وابسته‌ن.

   این ماژول idempotent و thread-safe هست؛ خودش هیچ‌وقت بلاک
   نمی‌کنه مگر در تابع shutdown_brake_wait_all که عمداً برای
   همین کار طراحی شده.
   ─────────────────────────────────────────────────────────────
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define SHUTDOWN_BRAKE_MAX_ENTRIES 128
#define SHUTDOWN_BRAKE_NAME_LEN    64

/* یه subsystem قبل از شروع کارش این تابع رو صدا می‌زنه.
   برمی‌گردونه: یه handle (id) که باید برای unregister استفاده بشه،
   یا -1 اگه ثبت‌نام fail بشه (مثلاً چون در حالت draining هستیم —
   یعنی برنامه داره می‌بنده و دیگه نباید کار جدید شروع بشه). */
int shutdown_brake_register(const char *name);

/* subsystem وقتی کارش تمومه (چه موفق چه با خطا) این رو صدا می‌زنه.
   id همون مقداریه که register برگردوند. */
void shutdown_brake_unregister(int id);

/* برنامه رو وارد حالت "draining" می‌کنه — از این لحظه به بعد،
   shutdown_brake_register همیشه -1 برمی‌گردونه (subsystem جدید
   دیگه اجازه‌ی شروع نداره). این باید اولین قدم در روال خروج
   برنامه باشه. */
void shutdown_brake_begin_drain(void);

/* منتظر می‌مونه همه‌ی subsystem های ثبت‌شده unregister بشن،
   حداکثر تا timeout_ms میلی‌ثانیه. اگه بعد از timeout هنوز
   subsystem فعالی باقی مونده بود، لیست کامل (اسم + مدت‌زمان
   فعال بودنشون) رو لاگ می‌کنه (سطح WARN) تا مشخص بشه دقیقاً
   کدوم قسمت برنامه مانع خروج تمیز شده — این خودش یه ابزار
   دیباگ ارزشمنده برای گیر کردن‌های آینده.
   برمی‌گردونه: TRUE اگه همه به‌موقع تموم شدن، FALSE اگه timeout
   خورد (و برنامه باید به‌هرحال ادامه بده به بستن، چون منتظر
   موندن ابدی خودش یه مشکل جدی‌تره). */
BOOL shutdown_brake_wait_all(DWORD timeout_ms);

/* برای دیباگ/UI: تعداد subsystem های فعلاً فعال */
int shutdown_brake_active_count(void);

/* ریست کامل — فقط برای تست‌ها، تا هر تست از یه state تازه شروع کنه */
void shutdown_brake_reset_for_tests(void);
