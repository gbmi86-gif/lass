/* version.h — نسخه‌ی واحد و مرکزی برنامه
   ─────────────────────────────────────────────────────────
   طبق درخواست کاربر: هر بار که تغییری (fix، feature، حتی
   کوچیک) اعمال میشه، PATCH باید افزایش پیدا کنه — این‌طوری
   همیشه مشخصه کدوم build داره تست میشه، بدون نیاز به حدس زدن
   یا مقایسه‌ی timestamp فایل‌ها.

   این تنها جایی‌ست که رشته‌ی نسخه تعریف میشه؛ همه‌ی فایل‌های
   دیگه (guo.c، resource.rc، browser_ext، تست‌ها) باید از همین
   ماکروها استفاده کنن، نه رشته‌ی hardcoded جدا.
   ─────────────────────────────────────────────────────────
*/
#pragma once

#define LASS_VERSION_MAJOR 5
#define LASS_VERSION_MINOR 0
#define LASS_VERSION_PATCH 57

/* رشته‌ی نمایشی کامل — این چیزیه که در Title bar، About dialog،
   و لاگ‌ها نشون داده میشه */
#define LASS_VERSION_STRING "5.0.57"

/* برای resource.rc که به فرمت عددی جدا (comma-separated) نیاز
   داره برای VERSIONINFO */
#define LASS_VERSION_RC 5,0,57,0
