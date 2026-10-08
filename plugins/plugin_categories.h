/* plugin_categories.h
   ─────────────────────────────────────────────────────────────
   Download Categories Plugin

   دسته‌بندی خودکار دانلود‌ها بر اساس پسوند یا URL pattern:
     Video    — mp4, mkv, avi, mov, wmv, flv, m3u8, mpd
     Audio    — mp3, flac, aac, ogg, wav, m4a
     Document — pdf, doc, docx, xls, ppt, txt, epub
     Software — exe, msi, dmg, apk, zip, rar, 7z, tar, gz
     Image    — jpg, png, gif, bmp, svg, webp
     Other    — بقیه

   ذخیره در lass.ini زیر [Categories]
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_PLUGIN_CATEGORIES_H
#define LASS_PLUGIN_CATEGORIES_H

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAT_MAX_CATEGORIES  16
#define CAT_MAX_EXTS        32
#define CAT_EXT_LEN         16
#define CAT_NAME_LEN        64
#define CAT_DIR_LEN         512

typedef struct {
    char name[CAT_NAME_LEN];         /* "Video", "Audio", ... */
    char save_dir[CAT_DIR_LEN];      /* پوشه ذخیره مخصوص — خالی = default */
    char exts[CAT_MAX_EXTS][CAT_EXT_LEN]; /* ".mp4", ".mkv", ... */
    int  ext_count;
    BOOL enabled;
} DL_Category;

/* ── ساخت/نابود ── */
IDM_PluginHooks *categories_plugin_create (const char *ini_path);
void             categories_plugin_destroy(IDM_PluginHooks *plug);

/* ── دسته‌بندی یه URL ── */
/* برمیگردونه ایندکس category (0-based) یا -1 اگه Other */
int  categories_classify  (IDM_PluginHooks *plug, const char *url,
                             const char *filename);

/* برمیگردونه نام category */
const char *categories_name(IDM_PluginHooks *plug, int cat_index);

/* پوشه مخصوص category — خالی اگه نداره */
const char *categories_dir (IDM_PluginHooks *plug, int cat_index);

/* تعداد category ها */
int  categories_count      (IDM_PluginHooks *plug);

/* دریافت category برای ویرایش */
BOOL categories_get        (IDM_PluginHooks *plug, int idx, DL_Category *out);
BOOL categories_set        (IDM_PluginHooks *plug, int idx,
                             const DL_Category *cat);

/* save/load از INI */
void categories_save       (IDM_PluginHooks *plug);
void categories_load       (IDM_PluginHooks *plug);

/* out_path رو بر اساس category تنظیم کن */
/* اگه category dir داشت مسیر رو override می‌کنه */
void categories_resolve_path(IDM_PluginHooks *plug,
                              const char *url,
                              const char *filename,
                              const char *default_dir,
                              char *out, int out_sz);

#ifdef __cplusplus
}
#endif

#endif /* LASS_PLUGIN_CATEGORIES_H */
