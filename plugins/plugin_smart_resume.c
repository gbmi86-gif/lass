/* plugin_smart_resume.c
   ─────────────────────────────────────────────────────────────
   Smart Resume Plugin
   ساختار فایل state:
     [SR_Header]
     [SR_SegState × num_segments]
   همه چیز XOR-encrypted با یه کلید ۸ بایتی
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../src/legacy_data.h"
#include "plugin_smart_resume.h"
#include "../src/lass_log.h"

/* ======================================================
   ثابت‌ها
   ====================================================== */
#include "lass_store.h"
#include "lass_paths.h"

#define SR_STORE_DIR "states"
#define SR_MAGIC    0x53524944u   /* 'IDRS' */
#define SR_VERSION  2
#define SR_MAX_SEGS 32

/* ======================================================
   ساختارهای state file
   ====================================================== */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t num_segments;
    char     url[IDM_MAX_URL];
    char     out_path[IDM_MAX_PATH];
    uint64_t total_size;
    uint64_t downloaded;
    uint64_t url_hash;       /* FNV-1a hash */
    uint64_t size_sig;       /* total_size XOR magic constant */
    char     etag[128];      /* برای تشخیص تغییر فایل روی سرور */
    char     last_modified[64];
    uint32_t checksum;       /* CRC32 ساده از کل هدر */
} SR_Header;

typedef struct {
    uint32_t id;
    uint64_t start;
    uint64_t end;
    uint64_t current;   /* از اینجا ادامه بده */
    uint32_t crc;       /* CRC32 این رکورد */
} SR_SegState;
#pragma pack(pop)

/* ======================================================
   داده خصوصی پلاگین
   ====================================================== */
typedef struct {
    char    state_dir[IDM_MAX_PATH];
    uint8_t xor_key[8];
    CRITICAL_SECTION lock;
} SR_Priv;

/* ======================================================
   ابزارها
   ====================================================== */

/* FNV-1a 64-bit */
static uint64_t fnv1a_64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* CRC32 ساده (polynomial 0xEDB88320) */
static uint32_t crc32_simple(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

/* XOR encrypt/decrypt در جا */
static void xor_crypt(void *data, size_t len, const uint8_t key[8]) {
    uint8_t *p = (uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        p[i] ^= key[i & 7];
}

/* مسیر فایل state
   پسوند ثابت: \sr_ + 16 hex + .dat = 25 کاراکتر
   پس state_dir باید حداکثر MAX_PATH-26 باشه */
#define SR_SUFFIX_LEN  25   /* \sr_0123456789abcdef.dat */

/* نام entry در container (نه مسیر فایل): sr_<hash(url) xor hash(out_path)> */
static void make_state_path(SR_Priv *priv, const char *url,
                             const char *out_path, char *out, int max) {
    (void)priv;
    if (max <= 0) return;
    if (!url || !out_path) { out[0] = '\0'; return; }
    uint64_t h = fnv1a_64(url) ^ fnv1a_64(out_path);
    snprintf(out, (size_t)max, "sr_%016llx", (unsigned long long)h);
}

/* ======================================================
   ذخیره state
   ====================================================== */
static void sr_save(SR_Priv *priv, IDM_Download *dl) {
    if (!priv || !dl) return;

    /* ساخت پوشه اگه وجود نداشت */
    CreateDirectoryA(priv->state_dir, NULL);

    char path[IDM_MAX_PATH];
    make_state_path(priv, dl->url, dl->out_path, path, sizeof(path));

    EnterCriticalSection(&priv->lock);

    /* ── هدر ── */
    SR_Header hdr = {0};
    hdr.magic        = SR_MAGIC;
    hdr.version      = SR_VERSION;
    hdr.num_segments = (uint16_t)(dl->num_segments < SR_MAX_SEGS
                                  ? dl->num_segments : SR_MAX_SEGS);
    snprintf(hdr.url,      IDM_MAX_URL,  "%s", dl->url);
    snprintf(hdr.out_path, IDM_MAX_PATH, "%s", dl->out_path);
    hdr.total_size  = dl->total_size;
    hdr.downloaded  = (uint64_t)dl->downloaded;
    hdr.url_hash    = fnv1a_64(dl->url);
    hdr.size_sig    = dl->total_size ^ 0x9e3779b97f4a7c15ULL;
    hdr.checksum    = 0;
    hdr.checksum    = crc32_simple(&hdr, sizeof(hdr));

    /* ── segment states ──
       دفاعی: dl->segments می‌تونه NULL باشه (همون کلاس race که در
       resume_save واقعاً crash داده بود) — قبل از استفاده چک کن. */
    SR_SegState segs[SR_MAX_SEGS] = {0};
    if (dl->segments) {
        for (uint16_t i = 0; i < hdr.num_segments; i++) {
            IDM_Segment *seg = dl->segments[i];
            if (!seg) continue;
            EnterCriticalSection(&seg->lock);
            segs[i].id      = seg->id;
            segs[i].start   = seg->start;
            segs[i].end     = seg->end;
            segs[i].current = seg->current;
            LeaveCriticalSection(&seg->lock);
            segs[i].crc = crc32_simple(&segs[i],
                                        sizeof(segs[i]) - sizeof(uint32_t));
        }
    }

    /* ── رمزگذاری ── */
    SR_Header hdr_enc = hdr;
    xor_crypt(&hdr_enc, sizeof(hdr_enc), priv->xor_key);

    size_t seg_sz = sizeof(SR_SegState) * hdr.num_segments;
    uint8_t *segs_enc = (uint8_t *)malloc(seg_sz);
    if (!segs_enc) {
        LeaveCriticalSection(&priv->lock);
        return;
    }
    memcpy(segs_enc, segs, seg_sz);
    xor_crypt(segs_enc, seg_sz, priv->xor_key);

    /* ── نوشتن در container (به‌جای فایل) ── */
    size_t blob_len = sizeof(hdr_enc) + seg_sz;
    uint8_t *blob = (uint8_t *)malloc(blob_len);
    if (blob) {
        memcpy(blob, &hdr_enc, sizeof(hdr_enc));
        memcpy(blob + sizeof(hdr_enc), segs_enc, seg_sz);
        if (!lass_store_put(SR_STORE_DIR, path, blob, (uint32_t)blob_len))
            LOGW(LOG_MOD_SMARTRESUME, "state could not be written to the store");
        free(blob);
    }

    free(segs_enc);
    LeaveCriticalSection(&priv->lock);
}

/* ======================================================
   بارگذاری state
   ====================================================== */
static bool sr_load(SR_Priv *priv, IDM_Download *dl) {
    if (!priv || !dl) return false;

    char path[IDM_MAX_PATH];
    make_state_path(priv, dl->url, dl->out_path, path, sizeof(path));

    uint8_t *blob = NULL;
    uint32_t blen = 0;
    if (!lass_store_get(SR_STORE_DIR, path, &blob, &blen))
        return false;

    EnterCriticalSection(&priv->lock);

    /* ── خواندن و رمزگشایی هدر ── */
    if (blen < sizeof(SR_Header)) {
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }
    SR_Header hdr_enc, hdr;
    memcpy(&hdr_enc, blob, sizeof(hdr_enc));
    memcpy(&hdr, &hdr_enc, sizeof(hdr));
    xor_crypt(&hdr, sizeof(hdr), priv->xor_key);

    /* ── اعتبارسنجی ── */
    if (hdr.magic != SR_MAGIC || hdr.version != SR_VERSION) {
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }

    /* بررسی CRC هدر */
    uint32_t saved_crc = hdr.checksum;
    hdr.checksum = 0;
    uint32_t calc_crc = crc32_simple(&hdr, sizeof(hdr));
    hdr.checksum = saved_crc;
    if (calc_crc != saved_crc) {
        LOGW(LOG_MOD_SMARTRESUME, "Header CRC mismatch - state corrupt");
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }

    /* URL باید match کنه */
    if (strcmp(hdr.url, dl->url) != 0) {
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }

    /* بررسی اینکه سرور فایل رو تغییر نداده */
    uint64_t expected_sig = hdr.total_size ^ 0x9e3779b97f4a7c15ULL;
    if (hdr.size_sig != expected_sig) {
        LOGW(LOG_MOD_SMARTRESUME, "Size signature mismatch - file changed");
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }

    /* ── خواندن segment‌ها ── */
    uint16_t nseg = hdr.num_segments;
    if (nseg == 0 || nseg > SR_MAX_SEGS) {
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }
    size_t seg_sz = sizeof(SR_SegState) * nseg;
    if ((size_t)blen != sizeof(SR_Header) + seg_sz) {
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc(seg_sz);
    if (!buf) {
        free(blob); LeaveCriticalSection(&priv->lock);
        return false;
    }
    memcpy(buf, blob + sizeof(SR_Header), seg_sz);
    free(blob);

    xor_crypt(buf, seg_sz, priv->xor_key);
    SR_SegState *segs = (SR_SegState *)buf;

    /* بررسی CRC هر segment */
    bool crc_ok = true;
    for (uint16_t i = 0; i < nseg; i++) {
        uint32_t sc = segs[i].crc;
        segs[i].crc = 0;
        if (crc32_simple(&segs[i], sizeof(segs[i]) - sizeof(uint32_t)) != sc) {
            crc_ok = false;
            LOGW(LOG_MOD_SMARTRESUME, "Segment %u CRC fail", i);
            break;
        }
        segs[i].crc = sc;
    }
    if (!crc_ok) {
        free(buf); LeaveCriticalSection(&priv->lock);
        return false;
    }

    /* ── پر کردن dl ── */
    dl->total_size   = hdr.total_size;
    dl->downloaded   = (LONGLONG)hdr.downloaded;
    dl->num_segments = nseg;

    /* resume_segs رو با داده‌های ما پر می‌کنیم */
    free(dl->resume_segs);
    dl->resume_segs = (IDM_ResumeSeg *)calloc(nseg, sizeof(IDM_ResumeSeg));
    if (dl->resume_segs) {
        for (uint16_t i = 0; i < nseg; i++) {
            dl->resume_segs[i].id      = segs[i].id;
            dl->resume_segs[i].start   = segs[i].start;
            dl->resume_segs[i].end     = segs[i].end;
            dl->resume_segs[i].current = segs[i].current;
        }
    }

    free(buf);
    LeaveCriticalSection(&priv->lock);

    LOGI(LOG_MOD_SMARTRESUME, "Loaded state: %llu/%llu bytes, %u segments",
            (unsigned long long)hdr.downloaded,
            (unsigned long long)hdr.total_size, nseg);
    return true;
}

/* ======================================================
   حذف فایل state
   ====================================================== */
static void sr_delete(SR_Priv *priv, const char *url, const char *out_path) {
    char path[IDM_MAX_PATH];
    make_state_path(priv, url, out_path, path, sizeof(path));
    lass_store_delete(SR_STORE_DIR, path);
}

/* ======================================================
   Hook callbacks
   ====================================================== */
static void hook_download_start(IDM_Download *dl) {
    IDM_PluginHooks *self = NULL;
    /* priv رو از context می‌گیریم – با جستجو در static pointer */
    /* ساده‌ترین روش: از dl->userdata استفاده نمی‌کنیم چون متعلق به کاربره
       به جاش از یه global static استفاده می‌کنیم */
    extern IDM_PluginHooks *g_sr_plugin_instance;
    self = g_sr_plugin_instance;
    if (!self) return;

    SR_Priv *priv = (SR_Priv *)self->priv;
    /* سعی کن state قبلی رو بارگذاری کنی */
    if (!dl->resume_segs) {
        sr_load(priv, dl);
    }
}

static void hook_download_done(IDM_Download *dl) {
    extern IDM_PluginHooks *g_sr_plugin_instance;
    IDM_PluginHooks *self = g_sr_plugin_instance;
    if (!self) return;
    SR_Priv *priv = (SR_Priv *)self->priv;
    /* دانلود کامل شد – state رو پاک کن */
    sr_delete(priv, dl->url, dl->out_path);
}

static void hook_download_pause(IDM_Download *dl) {
    extern IDM_PluginHooks *g_sr_plugin_instance;
    IDM_PluginHooks *self = g_sr_plugin_instance;
    if (!self) return;
    SR_Priv *priv = (SR_Priv *)self->priv;
    sr_save(priv, dl);
}

static void hook_download_fail(IDM_Download *dl) {
    extern IDM_PluginHooks *g_sr_plugin_instance;
    IDM_PluginHooks *self = g_sr_plugin_instance;
    if (!self) return;
    SR_Priv *priv = (SR_Priv *)self->priv;
    sr_save(priv, dl);
}

/* auto-save هر N chunk */
static void hook_segment_progress(IDM_Segment *seg, uint64_t bytes_written) {
    (void)bytes_written;
    /* هر ۵۱۲ chunk یه بار ذخیره کن */
    static volatile LONG counter = 0;
    if ((InterlockedIncrement(&counter) & 0x1FF) == 0) {
        extern IDM_PluginHooks *g_sr_plugin_instance;
        IDM_PluginHooks *self = g_sr_plugin_instance;
        if (!self) return;
        SR_Priv *priv = (SR_Priv *)self->priv;
        if (seg && seg->dl)
            sr_save(priv, seg->dl);
    }
}

static int hook_init(void) {
    LOGI(LOG_MOD_SMARTRESUME, "initialized");
    return 0;
}

static void hook_destroy(void) {
    LOGI(LOG_MOD_SMARTRESUME, "destroyed");
}

/* ======================================================
   Global instance pointer (یکی در هر پروسه)
   ====================================================== */
IDM_PluginHooks *g_sr_plugin_instance = NULL;

/* ======================================================
   Public API
   ====================================================== */
/* مهاجرت یک‌باره: فایل‌های sr_*.dat پوشه‌ی قدیمی در container کپی می‌شوند.
   فایل‌ها دست‌نخورده می‌مانند؛ اگر نامشان در container هست، کپی نمی‌شوند. */
static void sr_migrate_legacy_folder(const char *dir) {
    if (!dir || !dir[0]) return;
    size_t dlen = strlen(dir);
    char pattern[IDM_MAX_PATH];
    if (dlen + 12 >= sizeof(pattern)) return;
    memcpy(pattern, dir, dlen);
    memcpy(pattern + dlen, "\\sr_*.dat", 10);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t fl = strlen(fd.cFileName);
        if (fl <= 4 || fl - 4 > 63) continue;
        char name[64];
        memcpy(name, fd.cFileName, fl - 4);
        name[fl - 4] = '\0';
        if (lass_store_exists(SR_STORE_DIR, name)) continue;
        char full[IDM_MAX_PATH];
        if (dlen + 1 + fl + 1 > sizeof(full)) continue;
        memcpy(full, dir, dlen);
        full[dlen] = '\\';
        memcpy(full + dlen + 1, fd.cFileName, fl + 1);
        FILE *f = fopen(full, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= (1L << 20)) {
            uint8_t *b = (uint8_t *)malloc((size_t)sz);
            if (b && fread(b, 1, (size_t)sz, f) == (size_t)sz)
                lass_store_put(SR_STORE_DIR, name, b, (uint32_t)sz);
            free(b);
        }
        fclose(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

IDM_PluginHooks *smart_resume_plugin_create(const char *state_dir,
                                             const uint8_t xor_key[8]) {
    SR_Priv *priv = (SR_Priv *)calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (state_dir)
        snprintf(priv->state_dir, IDM_MAX_PATH, "%s", state_dir);
    else {
        /* پیش‌فرض: %APPDATA%\Lass\states، یا LASS_DATA_DIR در حالت تست */
        char dir[IDM_MAX_PATH] = {0};
        const char *sub = "\\states";
        size_t sl_ = strlen(sub);
        if (lass_data_dir(dir, sizeof(dir)) && strlen(dir) + sl_ < IDM_MAX_PATH) {
            size_t dl = strlen(dir);
            memcpy(priv->state_dir, dir, dl);
            memcpy(priv->state_dir + dl, sub, sl_ + 1);
        } else {
            strcpy(priv->state_dir, ".\\lass_states");
        }

        /* یک‌بار: فایل‌های .dat نسخه‌ی قدیمی؛ در حالت تست داده‌ی واقعی خوانده نمی‌شود */
        if (!lass_paths_test_mode()) {
            char appdata[IDM_MAX_PATH] = {0};
            const char *legacy_suffix = "\\IDM-Clone\\states";
            if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
                size_t al = strlen(appdata), ls = strlen(legacy_suffix);
                if (al + ls < IDM_MAX_PATH) {
                    char legacy[IDM_MAX_PATH] = {0};
                    memcpy(legacy, appdata, al);
                    memcpy(legacy + al, legacy_suffix, ls + 1);
                    legacy_copy_dat_dir_if_missing(legacy, priv->state_dir);
                }
            }
        }
    }

    sr_migrate_legacy_folder(priv->state_dir);

    if (xor_key)
        memcpy(priv->xor_key, xor_key, 8);
    else {
        /* کلید پیش‌فرض */
        uint8_t def_key[8] = {0x4B, 0x3F, 0xA2, 0x11, 0xDE, 0x77, 0x05, 0xC9};
        memcpy(priv->xor_key, def_key, 8);
    }

    InitializeCriticalSection(&priv->lock);

    IDM_PluginHooks *plugin = (IDM_PluginHooks *)calloc(1, sizeof(*plugin));
    if (!plugin) { free(priv); return NULL; }

    plugin->name              = "smart_resume";
    plugin->version           = "1.0";
    plugin->api_version       = IDM_PLUGIN_API_VERSION;
    plugin->priv              = priv;
    plugin->on_init           = hook_init;
    plugin->on_destroy        = hook_destroy;
    plugin->on_download_start = hook_download_start;
    plugin->on_download_done  = hook_download_done;
    plugin->on_download_pause = hook_download_pause;
    plugin->on_download_fail  = hook_download_fail;
    plugin->on_segment_progress = hook_segment_progress;

    g_sr_plugin_instance = plugin;
    return plugin;
}

void smart_resume_plugin_destroy(IDM_PluginHooks *plugin) {
    if (!plugin) return;
    SR_Priv *priv = (SR_Priv *)plugin->priv;
    if (priv) {
        DeleteCriticalSection(&priv->lock);
        free(priv);
    }
    if (g_sr_plugin_instance == plugin)
        g_sr_plugin_instance = NULL;
    free(plugin);
}

bool smart_resume_exists(IDM_PluginHooks *plugin,
                          const char *url, const char *out_path) {
    if (!plugin || !url || !out_path) return false;
    SR_Priv *priv = (SR_Priv *)plugin->priv;
    char path[IDM_MAX_PATH];
    make_state_path(priv, url, out_path, path, sizeof(path));
    return lass_store_exists(SR_STORE_DIR, path);
}

void smart_resume_delete(IDM_PluginHooks *plugin,
                          const char *url, const char *out_path) {
    if (!plugin || !url || !out_path) return;
    sr_delete((SR_Priv *)plugin->priv, url, out_path);
}
