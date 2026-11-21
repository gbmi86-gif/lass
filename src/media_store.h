/* media_store.h — کانتینر موقت (.tels)، نام فایل نهایی و اثر انگشت، مشترک HLS و DASH
   ─────────────────────────────────────────────────────────────
   - entry: هر آیتم کانتینر با نام یکتا در ریشه است؛ entry صفر بایتی نشانه‌ی کامل بودن است.
   - نام نهایی: پسوند منبع کنار گذاشته می‌شود، پسوند خروجی اضافه می‌شود، و اگر نام
     مشغول باشد «name (1).ext» گرفته می‌شود.
   - اثر انگشت: FNV-1a ۶۴ بیتی با همان ثابت‌های قبلی. این ثابت‌ها را تغییر نده؛
     state های ذخیره‌شده‌ی ادامه‌ی دانلود به آن‌ها وابسته‌اند.
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_MEDIA_STORE_H
#define LASS_MEDIA_STORE_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"
#include "tels_container.h"
#include "media_http.h"

/* ---- entry های کانتینر ---- */
TelsId         media_entry_find(TelsContainer *c, const char *name);          /* ۰ یعنی ندارد */
BOOL           media_entry_exists(TelsContainer *c, const char *name);
BOOL           media_entry_size(TelsContainer *c, const char *name, uint64_t *sz);
BOOL           media_entry_drop(TelsContainer *c, const char *name);           /* نبودنش TRUE است */
/* جایگزین کامل؛ داده در تکه‌های chunk بایتی نوشته می‌شود */
BOOL           media_entry_put(TelsContainer *c, const char *name, const uint8_t *data,
                               uint64_t len, uint32_t chunk);
BOOL           media_entry_marker(TelsContainer *c, const char *name);
/* داده‌ی کامل entry در out؛ اگر بزرگ‌تر از max_len باشد FALSE */
BOOL           media_entry_read(TelsContainer *c, const char *name, MediaBuf *out, uint64_t max_len);
/* نوشتن داده‌ی entry در یک handle فایل، با بافر buf به اندازه‌ی chunk */
BOOL           media_entry_copy_to_handle(TelsContainer *c, const char *name, HANDLE h,
                                          uint8_t *buf, uint32_t chunk);
BOOL           media_container_clear(TelsContainer *c);
void           media_container_finish(TelsContainer *c);
/* باز کردن کانتینر؛ اگر مشغول بود tries بار با خواب قابل توقف دوباره امتحان می‌کند */
TelsContainer *media_container_open(const char *path, uint32_t tries, const MediaHttp *h);

/* ---- نام فایل ---- */
BOOL           media_path_exists(const char *p);
BOOL           media_build_candidate(char *out, size_t sz, const char *stem, int n, const char *ext);
/* strip: پسوندهایی که باید کنار گذاشته شوند؛ ext: پسوند خروجی */
void           media_choose_final_path(const char *out_path, const char *ext,
                                       const char *const *strip, size_t n_strip,
                                       char *out, size_t sz);

/* ---- متادیتای resume ---- */
/* قالب: "<head>fp=<hex>[\nsegments=<n>]\nfinal=<name>\n"؛ بایت‌به‌بایت همان متن قبلی.
   اگر بافر کوچک باشد -1 برمی‌گرداند؛ متن نیمه‌کاره هرگز ذخیره نمی‌شود. */
int           media_meta_format(char *buf, size_t sz, const char *head, uint64_t fp,
                                BOOL has_segs, uint32_t segs, const char *final_name);
BOOL          media_meta_parse(const char *text, const char *head, BOOL has_segs,
                               uint64_t *fp, uint32_t *segs, char *final_name, size_t fn_sz);
BOOL          media_meta_load(TelsContainer *c, const char *name, const char *head, BOOL has_segs,
                              uint64_t *fp, uint32_t *segs, char *final_name, size_t fn_sz);

/* ---- اثر انگشت ---- */
uint64_t       media_fnv_mix(uint64_t h, const void *p, size_t n);
void           media_strip_query(const char *url, char *out, size_t sz);

#endif /* LASS_MEDIA_STORE_H */
