/* plugin_categories.c
   ─────────────────────────────────────────────────────────────
   Download Categories Plugin
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_categories.h"
#include "../src/lass_log.h"

#define CAT_PRIV_MAGIC 0x43415450u /* "CATP" ASCII */

typedef struct {
    DWORD        magic;   /* canary برای تشخیص heap corruption */
    DL_Category  cats[CAT_MAX_CATEGORIES];
    int          count;
    char         ini_path[512];
    CRITICAL_SECTION lock;
} Cat_Priv;

/* ── پیش‌فرض‌ها ── */
static const struct {
    const char *name;
    const char *exts;   /* comma-separated */
} DEFAULT_CATS[] = {
    {"Video",    ".mp4,.mkv,.avi,.mov,.wmv,.flv,.webm,.m4v,.3gp,.ts,.m3u8,.mpd"},
    {"Audio",    ".mp3,.flac,.aac,.ogg,.wav,.m4a,.wma,.opus,.amr"},
    {"Document", ".pdf,.doc,.docx,.xls,.xlsx,.ppt,.pptx,.txt,.epub,.mobi,.djvu"},
    {"Software", ".exe,.msi,.dmg,.apk,.zip,.rar,.7z,.tar,.gz,.bz2,.xz,.iso"},
    {"Image",    ".jpg,.jpeg,.png,.gif,.bmp,.svg,.webp,.tiff,.ico"},
};
#define DEFAULT_CAT_COUNT ((int)(sizeof(DEFAULT_CATS)/sizeof(DEFAULT_CATS[0])))

static void load_default_cats(Cat_Priv *p) {
    p->count = DEFAULT_CAT_COUNT;
    for (int i = 0; i < DEFAULT_CAT_COUNT; i++) {
        DL_Category *c = &p->cats[i];
        memset(c, 0, sizeof(*c));
        strncpy(c->name, DEFAULT_CATS[i].name, CAT_NAME_LEN - 1);
        c->enabled   = TRUE;
        c->ext_count = 0;

        /* parse comma-separated extensions
           نکته حیاتی: exts_buf باید explicit zero بشه قبل از
           strncpy، چون strncpy فقط n-1 بایت کپی می‌کنه و اگه
           source به اندازه یا بزرگ‌تر از بافر باشه، null
           terminator اضافه نمی‌کنه. بدون این memset، بایت آخر
           می‌تونست garbage بمونه و strtok از انتهای بافر عبور
           کنه (read/write overflow که می‌تونست به corruption
           تأخیری در یه پلاگین کاملاً نامرتبط منجر بشه). */
        char exts_buf[256] = {0};
        strncpy(exts_buf, DEFAULT_CATS[i].exts, sizeof(exts_buf)-1);
        exts_buf[sizeof(exts_buf)-1] = '\0'; /* تضمین مضاعف */
        char *tok = strtok(exts_buf, ",");
        while (tok && c->ext_count < CAT_MAX_EXTS) {
            strncpy(c->exts[c->ext_count], tok, CAT_EXT_LEN - 1);
            c->ext_count++;
            tok = strtok(NULL, ",");
        }
    }
}

/* ── پیدا کردن پسوند URL ── */
static void get_ext(const char *url, const char *filename,
                     char *out, int out_sz) {
    out[0] = '\0';
    const char *src = (filename && filename[0]) ? filename : url;
    if (!src) return;

    /* حذف query string */
    char clean[512] = {0};
    strncpy(clean, src, sizeof(clean) - 1);
    char *q = strchr(clean, '?'); if (q) *q = '\0';

    const char *dot = strrchr(clean, '.');
    if (!dot) return;
    strncpy(out, dot, out_sz - 1);

    /* lowercase */
    for (int i = 0; out[i]; i++)
        out[i] = (char)tolower((unsigned char)out[i]);
}

/* ── plugin hooks ── */
static int  cat_on_init   (void) { return 0; }
static void cat_on_destroy(void) {}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *categories_plugin_create(const char *ini_path) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    Cat_Priv *p = (Cat_Priv *)calloc(1, sizeof(*p));
    if (!p) { free(plug); return NULL; }

    InitializeCriticalSection(&p->lock);
    p->magic = CAT_PRIV_MAGIC;
    if (ini_path)
        strncpy(p->ini_path, ini_path, sizeof(p->ini_path) - 1);

    load_default_cats(p);

    /* بارگذاری از INI اگه وجود داشت — بعد از plug setup انجام میشه */

    plug->priv        = p;
    plug->name        = "categories";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = cat_on_init;
    plug->on_destroy  = cat_on_destroy;

    LOGI(LOG_MOD_CATEGORIES, "plugin created (%d categories)", p->count);

    /* بارگذاری از INI بعد از setup */
    if (p->ini_path[0])
        categories_load(plug);

    return plug;
}

void categories_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (p) {
        DeleteCriticalSection(&p->lock);
        free(p);
    }
    free(plug);
}

int categories_classify(IDM_PluginHooks *plug, const char *url,
                          const char *filename) {
    if (!plug) return -1;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p) return -1;

    char ext[32] = {0};
    get_ext(url, filename, ext, sizeof(ext));
    if (!ext[0]) return -1;

    EnterCriticalSection(&p->lock);
    int result = -1;
    for (int i = 0; i < p->count && result < 0; i++) {
        if (!p->cats[i].enabled) continue;
        for (int j = 0; j < p->cats[i].ext_count; j++) {
            if (_stricmp(ext, p->cats[i].exts[j]) == 0) {
                result = i; break;
            }
        }
    }
    LeaveCriticalSection(&p->lock);
    return result;
}

const char *categories_name(IDM_PluginHooks *plug, int idx) {
    if (!plug || idx < 0) return "Other";
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p || idx >= p->count) return "Other";
    return p->cats[idx].name;
}

const char *categories_dir(IDM_PluginHooks *plug, int idx) {
    if (!plug || idx < 0) return "";
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p || idx >= p->count) return "";
    return p->cats[idx].save_dir;
}

int categories_count(IDM_PluginHooks *plug) {
    if (!plug) return 0;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p) return 0;
    if (p->magic != CAT_PRIV_MAGIC) {
        LOGE(LOG_MOD_CATEGORIES,
             "FATAL: Cat_Priv magic mismatch "
             "(got 0x%08lX, expected 0x%08lX) - heap corruption!",
             (unsigned long)p->magic, (unsigned long)CAT_PRIV_MAGIC);
        return 0;
    }
    return p->count;
}

BOOL categories_get(IDM_PluginHooks *plug, int idx, DL_Category *out) {
    if (!plug || !out || idx < 0) return FALSE;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p || idx >= p->count) return FALSE;
    EnterCriticalSection(&p->lock);
    *out = p->cats[idx];
    LeaveCriticalSection(&p->lock);
    return TRUE;
}

BOOL categories_set(IDM_PluginHooks *plug, int idx,
                     const DL_Category *cat) {
    if (!plug || !cat || idx < 0) return FALSE;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p || idx >= p->count) return FALSE;
    EnterCriticalSection(&p->lock);
    p->cats[idx] = *cat;
    LeaveCriticalSection(&p->lock);
    return TRUE;
}

void categories_save(IDM_PluginHooks *plug) {
    if (!plug) return;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p || !p->ini_path[0]) return;

    EnterCriticalSection(&p->lock);
    for (int i = 0; i < p->count; i++) {
        char section[64];
        snprintf(section, sizeof(section), "Category_%d", i);

        WritePrivateProfileStringA(section, "name",
            p->cats[i].name, p->ini_path);
        WritePrivateProfileStringA(section, "dir",
            p->cats[i].save_dir, p->ini_path);
        WritePrivateProfileStringA(section, "enabled",
            p->cats[i].enabled ? "1" : "0", p->ini_path);

        /* extensions — comma-joined */
        char exts_str[512] = {0};
        for (int j = 0; j < p->cats[i].ext_count; j++) {
            if (j > 0) strncat(exts_str, ",",
                               sizeof(exts_str)-strlen(exts_str)-1);
            strncat(exts_str, p->cats[i].exts[j],
                    sizeof(exts_str)-strlen(exts_str)-1);
        }
        WritePrivateProfileStringA(section, "exts",
            exts_str, p->ini_path);
    }
    /* تعداد */
    char cnt[8]; snprintf(cnt, sizeof(cnt), "%d", p->count);
    WritePrivateProfileStringA("Categories", "count", cnt, p->ini_path);
    LeaveCriticalSection(&p->lock);
    LOGI(LOG_MOD_CATEGORIES, "saved to %s", p->ini_path);
}

void categories_load(IDM_PluginHooks *plug) {
    if (!plug) return;
    Cat_Priv *p = (Cat_Priv *)plug->priv;
    if (!p || !p->ini_path[0]) return;

    int cnt = (int)GetPrivateProfileIntA("Categories", "count", 0, p->ini_path);
    if (cnt <= 0 || cnt > CAT_MAX_CATEGORIES) return;

    EnterCriticalSection(&p->lock);
    p->count = cnt;
    for (int i = 0; i < cnt; i++) {
        char section[64];
        snprintf(section, sizeof(section), "Category_%d", i);

        GetPrivateProfileStringA(section, "name", p->cats[i].name,
            p->cats[i].name, CAT_NAME_LEN, p->ini_path);
        GetPrivateProfileStringA(section, "dir", "",
            p->cats[i].save_dir, CAT_DIR_LEN, p->ini_path);
        p->cats[i].enabled = (BOOL)GetPrivateProfileIntA(section,"enabled",1,p->ini_path);

        char exts_str[512] = {0};
        GetPrivateProfileStringA(section, "exts", "",
            exts_str, sizeof(exts_str), p->ini_path);

        p->cats[i].ext_count = 0;
        char *tok = strtok(exts_str, ",");
        while (tok && p->cats[i].ext_count < CAT_MAX_EXTS) {
            strncpy(p->cats[i].exts[p->cats[i].ext_count], tok, CAT_EXT_LEN-1);
            p->cats[i].ext_count++;
            tok = strtok(NULL, ",");
        }
    }
    LeaveCriticalSection(&p->lock);
    LOGI(LOG_MOD_CATEGORIES, "loaded %d categories", cnt);
}

/* ── ترکیب امنِ دایرکتوری + نام فایل ──
   یه gotcha واقعی و خیلی شناخته‌شده‌ی PathCombineA: صرف‌نظر از
   اینکه بافر مقصد چقدر بزرگه، خودِ PathCombineA یه سقف داخلی
   MAX_PATH (۲۶۰ کاراکتر) روی *نتیجه‌ی ترکیب‌شده* داره — اگه رد
   بشه، شکست می‌خوره و محتوای بافر مقصد مشخص نیست (ممکنه خالی
   بمونه). این دقیقاً می‌تونه با یه filename طولانیِ
   percent-encoded اتفاق بیفته — خیلی از سایت‌های ایرانی (از جمله
   mirror های دانلود) اسم فایل رو از عنوان فارسی می‌سازن، و هر حرف
   فارسی در URL-encoding می‌شه ۶ کاراکتر ASCII (%XX%XX)؛ یه عنوان
   ۴۰ حرفی فارسی به‌تنهایی می‌شه ۲۴۰+ کاراکتر، که به‌راحتی همراه با
   مسیر پوشه از ۲۶۰ رد می‌شه.
   این تابع قبل از PathCombineA طول رو چک می‌کنه؛ اگه جا نشه،
   filename رو (با حفظ پسوند در صورت معقول بودنش) کوتاه می‌کنه تا
   مطمئناً جا بشه — به‌جای تکیه به رفتار نامشخصِ PathCombineA روی
   شکست. */
static BOOL safe_path_combine(char *out, size_t out_sz,
                              const char *dir, const char *fname) {
    if (!out || out_sz == 0) return FALSE;
    out[0] = '\0';
    if (out_sz < (size_t)MAX_PATH) return FALSE; /* خودِ PathCombineA همین حداقل رو می‌خواد */
    if (!dir) dir = "";
    if (!fname || !fname[0]) fname = "download.bin";

    size_t dir_len = strlen(dir);
    size_t fn_len  = strlen(fname);
    size_t budget  = (size_t)MAX_PATH - 2; /* ۱ برای جداکننده، ۱ برای NUL */

    char fn_buf[MAX_PATH];
    if (dir_len + fn_len > budget) {
        size_t room = (budget > dir_len) ? (budget - dir_len) : 1;
        if (room >= sizeof(fn_buf)) room = sizeof(fn_buf) - 1;

        const char *dot = strrchr(fname, '.');
        size_t ext_len = (dot && strlen(dot) <= 10) ? strlen(dot) : 0;
        if (ext_len >= room) ext_len = 0;

        size_t base_room = room - ext_len;
        if (base_room < 1) base_room = 1;
        memcpy(fn_buf, fname, base_room);
        if (ext_len) memcpy(fn_buf + base_room, dot, ext_len);
        fn_buf[base_room + ext_len] = '\0';
        fname = fn_buf;
    }

    return PathCombineA(out, dir, fname) != NULL;
}

void categories_resolve_path(IDM_PluginHooks *plug,
                               const char *url,
                               const char *filename,
                               const char *default_dir,
                               char *out, int out_sz) {
    if (!out || out_sz <= 0) return;

    const char *use_dir = default_dir ? default_dir : "";

    /* category dir override */
    if (plug) {
        int cat = categories_classify(plug, url, filename);
        if (cat >= 0) {
            const char *cdir = categories_dir(plug, cat);
            if (cdir && cdir[0]) use_dir = cdir;
        }
    }

    /* نام فایل از filename یا URL */
    const char *fname = (filename && filename[0]) ? filename : NULL;
    if (!fname) {
        const char *sl = strrchr(url ? url : "", '/');
        fname = (sl && *(sl+1)) ? sl+1 : "download";
    }
    /* قبلاً: strncpy(fn_clean, fname, 255) و بعد strchr('?') — دو
       ضعف: (۱) برش از *انتها* بود، پس یه filename طولانی (مثلاً
       عنوان فارسی percent-encoded) پسوندش (.mp4) رو از دست
       می‌داد؛ (۲) اگه query string طولانی بود، خودِ '?' ممکن بود
       بیرون از ۲۵۵ بایت کپی‌شده بیفته و هرگز پیدا نشه. حالا اول
       طول بخش قبل از '?' رو حساب می‌کنیم، و اگه بیشتر از ظرفیته،
       از ابتدا برش می‌دیم ولی پسوند معقول (≤۱۰ کاراکتر) رو نگه
       می‌داریم. */
    char fn_clean[256] = {0};
    {
        size_t full_len = strcspn(fname, "?");
        if (full_len >= sizeof(fn_clean)) {
            const char *dot = NULL;
            for (size_t i = full_len; i > 0; i--)
                if (fname[i-1] == '.') { dot = fname + i - 1; break; }
            size_t ext_len = dot ? (size_t)(fname + full_len - dot) : 0;
            if (ext_len > 10) ext_len = 0;
            size_t base = sizeof(fn_clean) - 1 - ext_len;
            memcpy(fn_clean, fname, base);
            if (ext_len) memcpy(fn_clean + base, dot, ext_len);
            fn_clean[base + ext_len] = '\0';
        } else {
            memcpy(fn_clean, fname, full_len);
            fn_clean[full_len] = '\0';
        }
    }
    if (!fn_clean[0]) strncpy(fn_clean, "download", 9);

    /* اطمینان از وجود پوشه */
    if (use_dir[0])
        CreateDirectoryA(use_dir, NULL);

    if (use_dir[0])
        safe_path_combine(out, (size_t)out_sz, use_dir, fn_clean);
    else {
        char tmp[MAX_PATH];
        GetTempPathA(sizeof(tmp), tmp);
        safe_path_combine(out, (size_t)out_sz, tmp, fn_clean);
    }
    out[out_sz-1] = '\0';
}
