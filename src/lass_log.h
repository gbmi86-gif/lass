/* lass_log.h — سیستم لاگ‌گیری Lass Download Manager v5
   Thread-safe | File + stderr | Log rotation | 4 levels
*/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define LOG_MAX_SIZE     (5 * 1024 * 1024)
#define LOG_MAX_BACKUPS  3
#define LOG_MAX_LINE     1024

typedef enum { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERROR=3, LOG_NONE=4 } LogLevel;

void log_init     (const char *log_path, LogLevel min_level);
void log_destroy  (void);
void log_write    (LogLevel level, const char *module, const char *fmt, ...);

/* قفل مستقل خروجی کنسول — برای هماهنگی printf/fputs بین
   thread های مختلف. کد دیگه‌ای (مثل test_all.c) هم می‌تونه
   دور چاپ‌های خودش به stdout استفاده کنه تا با لاگ‌های
   background thread ها به‌هم نریزه. همیشه امنه، بدون نیاز به
   log_init قبلی. */
void log_console_lock(void);
void log_console_unlock(void);
void log_set_level(LogLevel level);
void log_flush    (void);

#define LOGD(mod,...) log_write(LOG_DEBUG,mod,__VA_ARGS__)
#define LOGI(mod,...) log_write(LOG_INFO, mod,__VA_ARGS__)
#define LOGW(mod,...) log_write(LOG_WARN, mod,__VA_ARGS__)
#define LOGE(mod,...) log_write(LOG_ERROR,mod,__VA_ARGS__)

#define LOG_MOD_ENGINE   "engine"
#define LOG_MOD_HTTP     "http"
#define LOG_MOD_STORAGE  "storage"
#define LOG_MOD_RESUME   "resume"
#define LOG_MOD_SEGMENT  "segment"
#define LOG_MOD_CORE     "core"
#define LOG_MOD_GUO      "guo"
#define LOG_MOD_QUEUE    "queue"
#define LOG_MOD_SETTINGS "settings"
#define LOG_MOD_EXTBR    "browser_ext"
#define LOG_MOD_NOTIFY   "notify"
#define LOG_MOD_SNIFFER  "video_sniffer"
#define LOG_MOD_PROXY    "proxy"
#define LOG_MOD_PLUGIN_API "plugin_api"
#define LOG_MOD_PLUGINS  "plugins"
#define LOG_MOD_BASKET   "basket"
#define LOG_MOD_CATEGORIES "categories"
#define LOG_MOD_CHMML    "chmml"
#define LOG_MOD_CLIPBOARD "clipboard"
#define LOG_MOD_COOKIE   "cookie_jar"
#define LOG_MOD_DASH     "dash"
#define LOG_MOD_FTP      "ftp"
#define LOG_MOD_AUTH     "http_auth"
#define LOG_MOD_SMARTRESUME "smart_resume"
#define LOG_MOD_HLS      "hls"
