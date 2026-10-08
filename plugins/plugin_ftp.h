/* plugin_ftp.h
   ─────────────────────────────────────────────────────────────
   FTP/FTPS Download Plugin

   پشتیبانی از:
     ftp://user:pass@host/path/file.zip
     ftps://host/path/file.zip  (FTP over SSL)
     ftp://host/path/            (directory listing)

   از WinINet FTP API استفاده می‌کنه — بدون libcurl

   ⚠️ وضعیت (بررسی‌شده تا v5.0.5): این پلاگین ساخته و attach
   می‌شه، ولی مسیر اصلی دانلود (src/http.c) به هیچ‌وجه بهش وصل
   نیست — InternetConnectA در http.c با INTERNET_SERVICE_HTTP
   هاردکد شده و هیچ‌جا چک نمی‌کنه URL از نوع ftp:// هست یا نه.
   یعنی الان اگه کاربر یه لینک ftp:// بده، تلاش برای اتصال با
   API های HTTP انجام می‌شه که کار نمی‌کنه. وصل کردن واقعیش نیاز
   به شاخه‌بندی پروتکل در http.c/segment.c داره (FTP نه Range
   header داره، نه همون مکانیزم resume) — یه تغییر جدا و
   قابل‌توجه، نه یه فیکس کوچیک.
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_FTP_H
#define IDM_PLUGIN_FTP_H

#include "plugin_api.h"
#include "../src/http.h"   /* HTTP_DiscoveryInfo, HTTP_Conn */

#ifdef __cplusplus
extern "C" {
#endif

/* بررسی اینکه URL از نوع FTP هست */
BOOL ftp_is_ftp_url(const char *url);

/* همان interface که engine استفاده می‌کنه */
IDM_Result ftp_discover(const char *url,
                         const char *user,
                         const char *pass,
                         HTTP_DiscoveryInfo *out);

/* باز کردن connection برای دانلود — range اگه سرور support کنه */
HTTP_Conn *ftp_open(const char *url,
                    const char *user,
                    const char *pass,
                    uint64_t    resume_from);

/* plugin lifecycle */
IDM_PluginHooks *ftp_plugin_create(void);
void             ftp_plugin_destroy(IDM_PluginHooks *plug);

#ifdef __cplusplus
}
#endif

#endif /* IDM_PLUGIN_FTP_H */
