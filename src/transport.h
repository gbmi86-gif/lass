/* transport.h — لایه‌ی انتقال یکپارچه برای موتور

   موتور فقط با این لایه کار می‌کند؛ انتخاب HTTP یا FTP بر اساس scheme شِ URL
   انجام می‌شود. http.c و plugin_ftp.c دست‌نخورده باقی می‌مانند. */
#ifndef LASS_TRANSPORT_H
#define LASS_TRANSPORT_H

#include <windows.h>
#include <stdint.h>
#include "types.h"
#include "http.h"

typedef struct DlStream DlStream;

/* URL پشتیبانی‌شده است؟ (http, https، و ftp بدون ftps) */
BOOL       dl_url_supported(const char *url);
/* URL اتصال: اگر رمز در حافظه باشد همان (secret_url)، وگرنه URL ذخیره‌شده */
const char *dl_conn_url(const struct IDM_Download *dl);
/* اطلاعات اولیه‌ی دانلود (اندازه، نام، پشتیبانی بازه) */
IDM_Result dl_discover(const char *url, const char *referer,
                       struct IDM_Download *dl, HTTP_DiscoveryInfo *out);
/* بازه‌ی شامل [start, end] (end=0 یعنی تا انتها) */
DlStream  *dl_stream_open_range(struct IDM_Download *dl, uint64_t start, uint64_t end);
/* >0 بایت، 0 پایان، <0 خطا */
int        dl_stream_read(DlStream *s, void *buf, int buf_size);
BOOL       dl_stream_got_partial(const DlStream *s);
void       dl_stream_close(DlStream *s);

#endif
