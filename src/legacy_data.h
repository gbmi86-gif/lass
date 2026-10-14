/* legacy_data.h — انتقال یک‌باره‌ی داده‌های کاربر از پوشه‌ی نسخه‌ی قدیمی
   (%APPDATA%\IDM-Clone) به پوشه‌ی فعلی (%APPDATA%\Lass).

   فقط برای داده‌هایی که نسخه‌ی قدیمی در مسیر قدیمی ذخیره می‌کرد
   (کوکی‌ها و state های Smart Resume). قواعد:
     - فایل مقصد هرگز بازنویسی نمی‌شود.
     - فایل قدیمی هرگز پاک یا تغییر نمی‌کند.
     - اگر منبع وجود نداشته باشد، هیچ کاری انجام نمی‌شود. */
#ifndef LASS_LEGACY_DATA_H
#define LASS_LEGACY_DATA_H

/* اگر current_path وجود نداشته باشد و legacy_path وجود داشته باشد،
   فایل را کپی می‌کند. برمی‌گرداند 1 اگر کپی شد، در غیر این صورت 0. */
int legacy_copy_file_if_missing(const char *legacy_path, const char *current_path);

/* تمام فایل‌های *.dat داخل legacy_dir را که در current_dir نیستند کپی می‌کند.
   برمی‌گرداند تعداد فایل‌های کپی‌شده. */
int legacy_copy_dat_dir_if_missing(const char *legacy_dir, const char *current_dir);

#endif
