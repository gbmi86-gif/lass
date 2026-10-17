/* lass_paths.h — مسیرهای داده و کلیدهای رجیستری برنامه

   به‌طور پیش‌فرض همه‌ی داده‌ها زیر %APPDATA%\Lass است. اگر متغیر محیطی LASS_DATA_DIR
   ست باشد، همان پوشه به‌جای APPDATA استفاده می‌شود و کلید رجیستری، نام استارت و نام
   pipe مرورگر هم به نسخه‌ی آزمایشی می‌روند. تست‌ها همیشه با این متغیر اجرا می‌شوند تا
   داده‌ی واقعی کاربر (ini، cookies، فروشگاه، رجیستری، استارت ویندوز) دست‌نخورده بماند. */
#ifndef LASS_PATHS_H
#define LASS_PATHS_H

#include <windows.h>
#include <stddef.h>

BOOL        lass_paths_test_mode(void);                 /* TRUE اگر LASS_DATA_DIR ست باشد */
BOOL        lass_data_dir(char *out, size_t out_sz);    /* بدون \ انتهایی */
const char *lass_reg_settings_key(void);               /* HKCU\... برای تنظیمات */
const char *lass_reg_startup_value(void);              /* نام مقدار استارت در Run */
const char *lass_pipe_name(void);                      /* \\.\pipe\... مرورگر */
BOOL        lass_reg_vendor_path(const char *prod_path, char *out, size_t out_sz); /* Software\X → Software\LassTest\X در تست */
const char *lass_reg_browser_ext_key(void);           /* کلید تنظیمات افزونه‌ی مرورگر */

#endif /* LASS_PATHS_H */
