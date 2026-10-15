/* plugin_ftp.h — کلاینت FTP مستقل (WinSock، بدون WinINet)

   پشتیبانی:
     - ftp://[user[:pass]@]host[:port]/path/file  (پیش‌فرض: anonymous)
     - SIZE برای اندازه‌ی فایل؛ REST برای دانلود بازه‌ای و ادامه‌ی دانلود
     - حالت PASV (سازگار با فایروال‌ها)
   محدودیت‌های صریح:
     - ftps:// (TLS) پشتیبانی نمی‌شود و به‌عنوان URL پشتیبانی‌شده رد می‌شود
     - IPv6 literal در URL پشتیبانی نمی‌شود
     - هر بازه‌ی دانلود اتصال کنترل مستقل دارد */
#ifndef LASS_PLUGIN_FTP_H
#define LASS_PLUGIN_FTP_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include "../src/types.h"
#include "plugin_api.h"

typedef struct FtpStream FtpStream;

/* URL با ftp:// یا ftps:// شروع شود */
BOOL       ftp_is_ftp_url(const char *url);
/* ftp:// معتبر با میزبان و نام فایل (ftps رد می‌شود) */
BOOL       ftp_url_supported(const char *url);
/* اندازه، نام فایل و پشتیبانی REST را برمی‌گرداند */
IDM_Result ftp_discover(const char *url, uint64_t *size_out,
                        char *filename_out, size_t filename_sz,
                        BOOL *accept_ranges_out);
/* اتصال، ورود، REST/RETR؛ بازه‌ی شامل [start, end] (end=0 یعنی تا انتها) */
FtpStream *ftp_range_open(const char *url, uint64_t start, uint64_t end);
/* >0 بایت خوانده‌شده، 0 پایان بازه، <0 خطا (از جمله قطع زودهنگام) */
int        ftp_range_read(FtpStream *st, void *buf, int buf_size);
BOOL       ftp_range_got_partial(const FtpStream *st);
void       ftp_range_close(FtpStream *st);

IDM_PluginHooks *ftp_plugin_create(void);
void             ftp_plugin_destroy(IDM_PluginHooks *plug);

#endif
