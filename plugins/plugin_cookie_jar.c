/* plugin_cookie_jar.c
   ─────────────────────────────────────────────────────────────
   Cookie Jar Plugin
   فرمت ذخیره: Netscape cookies.txt (سازگار با curl/wget)
   Domain matching: suffix match (x.example.com ← .example.com)
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
#include <time.h>
#include <stdbool.h>
#include "plugin_cookie_jar.h"
#include "../src/lass_log.h"

/* ======================================================
   ساختارها
   ====================================================== */
#define CJ_MAX_COOKIES   256
#define CJ_NAME_LEN      128
#define CJ_VALUE_LEN     512
#define CJ_DOMAIN_LEN    128
#define CJ_PATH_LEN      256

typedef struct {
    char   domain[CJ_DOMAIN_LEN];
    char   path[CJ_PATH_LEN];
    char   name[CJ_NAME_LEN];
    char   value[CJ_VALUE_LEN];
    time_t expires;      /* 0 = session cookie */
    time_t set_at;       /* برای eviction LRU وقتی جار پره و هیچ‌کدوم
                            expire نشده — رجوع کن به cj_set_full */
    bool   http_only;
    bool   secure;
    bool   active;
} CJ_Cookie;

/* ── کپی امن به بافر ثابت ──
   جایگزین snprintf(dst, dst_sz, "%s", src) در همه‌ی این فایل.
   هر دو دقیقاً یه کار می‌کنن (truncate امن، همیشه null-terminated)،
   ولی وقتی src یه اشاره‌گر بدون سایز شناخته‌شده در compile-time
   باشه (مثلاً یه پارامتر const char* که از یه بافر خیلی بزرگ‌تر
   مثل work[1024] میاد) و dst یه آرایه‌ی محلی کوچیک‌تر، GCC جدیدتر
   (دقیقاً چیزی که Juliya با MSYS2/UCRT64 دید) -Wformat-truncation
   می‌ده — چون نمی‌تونه در compile-time ثابت کنه truncation
   نمی‌افته، حتی وقتی رفتار واقعی کاملاً امنه. memcpy+strlen این
   کلاس کامل warning رو از ریشه حذف می‌کنه، نه فقط یه مورد خاص. */
static void safe_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

typedef struct {
    CJ_Cookie        cookies[CJ_MAX_COOKIES];
    int              count;
    char             cookie_file[IDM_MAX_PATH];
    CRITICAL_SECTION lock;
    bool             dirty;
} CJ_Priv;

IDM_PluginHooks *g_cj_plugin_instance = NULL;

/* ======================================================
   ابزارها
   ====================================================== */

/* استخراج domain از URL */
static void extract_domain(const char *url, char *out, int max) {
    out[0] = '\0';
    const char *p = url;
    if (strncmp(p, "http://",  7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* استخراج path از URL (شامل leading /، بدون query string) —
   برای path-scoping کوکی‌ها لازمه (Set-Cookie با Path=/foo نباید
   برای درخواست‌های /bar فرستاده بشه). اگه URL هیچ path ای نداشت
   (مثلاً "https://host"), پیش‌فرض "/" برمی‌گردونه — دقیقاً همون
   چیزی که یه مرورگر واقعی هم می‌فرسته. */
static void extract_path(const char *url, char *out, int max) {
    out[0] = '\0';
    if (max <= 1) return;
    const char *p = url;
    if (strncmp(p, "http://",  7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    const char *slash = strchr(p, '/');
    if (!slash) { strcpy(out, "/"); return; }
    int i = 0;
    while (slash[i] && slash[i] != '?' && slash[i] != '#' && i < max - 1) {
        out[i] = slash[i];
        i++;
    }
    out[i] = '\0';
    if (out[0] == '\0') strcpy(out, "/");
}

static bool is_https_url(const char *url) {
    return url && _strnicmp(url, "https://", 8) == 0;
}

/* domain matching: cookie.domain می‌تونه prefix با . باشه */
static bool domain_matches(const char *cookie_domain, const char *request_domain) {
    if (!cookie_domain || !request_domain) return false;
    if (cookie_domain[0] == '.') {
        /* suffix match */
        size_t cd_len = strlen(cookie_domain + 1);
        size_t rd_len = strlen(request_domain);
        if (rd_len < cd_len) return false;
        return (_stricmp(request_domain + rd_len - cd_len,
                         cookie_domain + 1) == 0);
    }
    return (_stricmp(cookie_domain, request_domain) == 0);
}

/* path matching طبق RFC 6265 §5.1.4 (نسخه‌ی ساده‌شده): cookie_path
   باید prefix واقعی request_path باشه — یعنی یا کاملاً برابرن، یا
   cookie_path با '/' تموم میشه، یا کاراکتر بعدی در request_path
   دقیقاً بعد از این prefix یه '/' هست. بدون این چک، یه کوکی که
   سرور صراحتاً Path=/account ست کرده بود برای هر درخواست دیگه‌ای
   به همون domain (حتی /images/logo.png) هم فرستاده می‌شد. */
static bool path_matches(const char *cookie_path, const char *request_path) {
    if (!cookie_path || !cookie_path[0]) return true; /* بدون path ثبت‌شده = بدون محدودیت */
    if (!request_path || !request_path[0]) request_path = "/";

    size_t cp_len = strlen(cookie_path);
    if (strncmp(cookie_path, request_path, cp_len) != 0) return false;
    if (request_path[cp_len] == '\0') return true;               /* دقیقاً برابر */
    if (cookie_path[cp_len - 1] == '/') return true;              /* خودش با / تموم میشه */
    return (request_path[cp_len] == '/');                         /* مرز segment درست */
}

/* ======================================================
   CRUD
   ====================================================== */
/* cj_set_full: نسخه‌ی کامل که path/secure/http_only رو هم می‌گیره
   — برای Set-Cookie واقعی از سرور. مطابق (match) بر اساس
   domain+path+name باهم، نه فقط domain+name — چون طبق RFC 6265
   دو کوکی با اسم یکسان ولی path متفاوت روی همون domain، دو کوکی
   جدان، نه یکی. */
static void cj_set_full(CJ_Priv *priv,
                         const char *domain, const char *path,
                         const char *name,   const char *value,
                         time_t expires, bool secure, bool http_only) {
    if (!priv || !domain || !name || !value) return;
    if (!path || !path[0]) path = "/";

    EnterCriticalSection(&priv->lock);
    time_t now = time(NULL);

    /* آپدیت اگه دقیقاً همین domain+path+name از قبل هست */
    for (int i = 0; i < priv->count; i++) {
        CJ_Cookie *c = &priv->cookies[i];
        if (c->active &&
            _stricmp(c->domain, domain) == 0 &&
            strcmp(c->path, path) == 0 &&
            strcmp(c->name, name) == 0) {
            safe_copy(c->value, CJ_VALUE_LEN, value);
            c->expires   = expires;
            c->secure    = secure;
            c->http_only = http_only;
            c->set_at    = now;
            priv->dirty  = true;
            LeaveCriticalSection(&priv->lock);
            return;
        }
    }

    /* اضافه کردن جدید — اگه جار پره: */
    if (priv->count >= CJ_MAX_COOKIES) {
        /* اولویت ۱: هر کوکی واقعاً expire شده رو حذف کن */
        int victim = -1;
        for (int i = 0; i < priv->count; i++) {
            if (priv->cookies[i].expires > 0 && priv->cookies[i].expires < now) {
                victim = i;
                break;
            }
        }
        /* اولویت ۲ (نکته‌ی جدید — قبلاً این حالت اصلاً پوشش داده
           نمی‌شد و جار پر از کوکی‌های زنده‌ی طولانی‌مدت به‌سادگی
           برای همیشه از قبول کوکی تازه امتناع می‌کرد، حتی وقتی
           همون کوکی تازه دقیقاً چیزیه که یه دانلود جدید بهش نیاز
           داره): اگه هیچ‌کدوم expire نشده، قدیمی‌ترین (بر اساس
           set_at) رو evict کن — رفتار LRU معمول هر cookie jar
           واقعی (مرورگرها همین کار رو می‌کنن). */
        if (victim < 0) {
            time_t oldest = now + 1;
            for (int i = 0; i < priv->count; i++) {
                if (priv->cookies[i].set_at < oldest) {
                    oldest = priv->cookies[i].set_at;
                    victim = i;
                }
            }
        }
        if (victim >= 0) {
            priv->count--;
            for (int j = victim; j < priv->count; j++)
                priv->cookies[j] = priv->cookies[j + 1];
        } else {
            LeaveCriticalSection(&priv->lock);
            return; /* عملاً غیرممکنه (count>0 تضمینیه) ولی دفاعی */
        }
    }

    CJ_Cookie *c = &priv->cookies[priv->count++];
    memset(c, 0, sizeof(*c));
    safe_copy(c->domain, CJ_DOMAIN_LEN, domain);
    safe_copy(c->path, CJ_PATH_LEN, path);
    safe_copy(c->name, CJ_NAME_LEN, name);
    safe_copy(c->value, CJ_VALUE_LEN, value);
    c->expires   = expires;
    c->secure    = secure;
    c->http_only = http_only;
    c->set_at    = now;
    c->active    = true;
    priv->dirty  = true;

    LeaveCriticalSection(&priv->lock);
}

/* wrapper سازگار با API عمومی قدیمی cookie_jar_set — host-only
   domain، path پیش‌فرض "/"، نه secure نه http_only. max_age_sec
   همون قرارداد قبلی رو داره: 0=session، >0=آینده، <0=فوراً expire. */
static void cj_set_internal(CJ_Priv *priv,
                              const char *domain, const char *name,
                              const char *value,  int max_age_sec) {
    time_t expires = (max_age_sec > 0)  ? (time_t)(time(NULL) + max_age_sec)
                    : (max_age_sec < 0)  ? (time_t)(time(NULL) - 1)
                    :                      0;
    cj_set_full(priv, domain, "/", name, value, expires, false, false);
}

/* ======================================================
   ساخت Cookie header برای یه درخواست مشخص (domain+path+scheme)
   ====================================================== */
static int cj_build_header(CJ_Priv *priv, const char *domain,
                             const char *path, bool is_https,
                             char *buf, int max_len) {
    if (!priv || !domain || !buf || max_len <= 0) return 0;

    EnterCriticalSection(&priv->lock);

    time_t now   = time(NULL);
    int    pos   = 0;
    bool   first = true;

    for (int i = 0; i < priv->count; i++) {
        CJ_Cookie *c = &priv->cookies[i];
        if (!c->active) continue;
        if (c->expires > 0 && c->expires < now) {
            c->active = false;
            priv->dirty = true;
            continue;
        }
        if (!domain_matches(c->domain, domain)) continue;
        if (!path_matches(c->path, path)) continue;
        /* Secure: هرگز روی HTTP ساده نفرست — قبلاً این پرچم اصلاً
           چک نمی‌شد، یعنی یه کوکی session که سرور صراحتاً Secure
           ست کرده بود، اگه (مثلاً به‌خاطر یه redirect یا mirror
           HTTP) درخواست بعدی روی http:// بود، درست مثل بقیه
           می‌رفت — دقیقاً همون افشایی که Secure برای جلوگیریش
           طراحی شده. */
        if (c->secure && !is_https) continue;

        int needed = (int)(strlen(c->name) + strlen(c->value) + 3);
        if (pos + needed >= max_len - 10) break;

        if (!first) { buf[pos++] = ';'; buf[pos++] = ' '; }
        first = false;
        pos += snprintf(buf + pos, max_len - pos, "%s=%s", c->name, c->value);
    }
    buf[pos] = '\0';

    LeaveCriticalSection(&priv->lock);
    return pos;
}

/* ======================================================
   ذخیره/بارگذاری Netscape format
   فرمت:  domain  flag  path  secure  expires  name  value
   ====================================================== */
static void cj_save_file(CJ_Priv *priv) {
    if (!priv || !priv->dirty || !priv->cookie_file[0]) return;

    EnterCriticalSection(&priv->lock);

    FILE *f = fopen(priv->cookie_file, "w");
    if (!f) { LeaveCriticalSection(&priv->lock); return; }

    fprintf(f, "# Netscape HTTP Cookie File\n");
    fprintf(f, "# IDM-Clone Cookie Jar\n\n");

    time_t now = time(NULL);
    for (int i = 0; i < priv->count; i++) {
        CJ_Cookie *c = &priv->cookies[i];
        if (!c->active) continue;
        if (c->expires > 0 && c->expires < now) continue;

        fprintf(f, "%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
                c->domain,
                (c->domain[0] == '.') ? "TRUE" : "FALSE",
                c->path[0] ? c->path : "/",
                c->secure ? "TRUE" : "FALSE",
                (long long)c->expires,
                c->name,
                c->value);
    }
    fclose(f);
    priv->dirty = false;
    LeaveCriticalSection(&priv->lock);
}

static void cj_load_file(CJ_Priv *priv) {
    if (!priv || !priv->cookie_file[0]) return;

    FILE *f = fopen(priv->cookie_file, "r");
    if (!f) return;

    char line[CJ_DOMAIN_LEN + CJ_PATH_LEN + CJ_NAME_LEN + CJ_VALUE_LEN + 64];
    time_t now = time(NULL);

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char domain[CJ_DOMAIN_LEN], flag[8], path[CJ_PATH_LEN];
        char secure[8], name[CJ_NAME_LEN], value[CJ_VALUE_LEN];
        long long expires = 0;

        int n = sscanf(line, "%127s %7s %255s %7s %lld %127s %511s",
                       domain, flag, path, secure, &expires, name, value);
        if (n < 6) continue;
        if (n == 6) value[0] = '\0';

        if (expires > 0 && (time_t)expires < now) continue;

        if (priv->count < CJ_MAX_COOKIES) {
            CJ_Cookie *c = &priv->cookies[priv->count++];
            memset(c, 0, sizeof(*c));
            safe_copy(c->domain, CJ_DOMAIN_LEN, domain);
            safe_copy(c->path, CJ_PATH_LEN, path);
            safe_copy(c->name, CJ_NAME_LEN, name);
            safe_copy(c->value, CJ_VALUE_LEN, value);
            c->expires   = (time_t)expires;
            c->secure    = (_stricmp(secure, "TRUE") == 0);
            c->http_only = false;
            c->set_at    = now; /* رفتار LRU معقول برای ورودی‌های تازه‌بارگذاری‌شده */
            c->active    = true;
        }
    }
    fclose(f);
    LOGI(LOG_MOD_COOKIE, "loaded %d cookies from %s",
         priv->count, priv->cookie_file);
}

/* ======================================================
   Hooks
   ====================================================== */
static int hook_on_http_headers(IDM_Download *dl, char *buf, int max_len) {
    if (!g_cj_plugin_instance || !dl) return 0;
    CJ_Priv *priv = (CJ_Priv *)g_cj_plugin_instance->priv;

    char domain[CJ_DOMAIN_LEN];
    extract_domain(dl->url, domain, sizeof(domain));
    if (!domain[0]) return 0;

    char path[CJ_PATH_LEN];
    extract_path(dl->url, path, sizeof(path));
    bool https = is_https_url(dl->url);

    char cookie_vals[2048];
    int n = cj_build_header(priv, domain, path, https,
                            cookie_vals, sizeof(cookie_vals));
    if (n <= 0) return 0;

    /* فرمت header کامل */
    int written = snprintf(buf, max_len, "Cookie: %s\r\n", cookie_vals);
    return (written > 0 && written < max_len) ? written : 0;
}

/* پارس یه تاریخ HTTP-date به فرمت RFC 1123 (تنها فرمتی که RFC 6265
   برای Expires مجاز می‌دونه و عملاً تنها فرمتی که سرورهای واقعی
   می‌فرستن): "Sun, 06 Nov 1994 08:49:37 GMT". عمداً از یه parser
   دستی و خودکفا استفاده می‌کنیم به‌جای InternetTimeToSystemTimeA
   (که در بعضی نسخه‌های headerهای mingw-w64 اصلاً declare نشده و
   implicit-declaration warning می‌ده) — این نسخه به هیچ چیزی
   بیرون از <time.h> استاندارد وابسته نیست. اگه پارس شکست بخوره
   (فرمت غیرمنتظره)، 0 برمی‌گردونه یعنی «session cookie» — امن‌ترین
   fallback، نه یه تاریخ اشتباه. */
static time_t parse_http_date(const char *s) {
    static const char *MONTHS[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    char mon[8] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;

    /* "%*[^,], %d %3s %d %d:%d:%d" رد کردن روز هفته + کاما، بعد
       DD Mon YYYY HH:MM:SS — تحمل‌پذیر نسبت به فاصله‌ی اضافه چون
       sscanf با %d/%3s خودش whitespace رو skip می‌کنه. */
    if (sscanf(s, "%*[^,], %d %7s %d %d:%d:%d",
               &day, mon, &year, &hh, &mm, &ss) != 6) {
        return 0;
    }

    int month = -1;
    for (int i = 0; i < 12; i++) {
        if (_stricmp(mon, MONTHS[i]) == 0) { month = i; break; }
    }
    if (month < 0 || day < 1 || day > 31 || year < 1970) return 0;

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon  = month;
    t.tm_mday = day;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    t.tm_isdst = 0;

    time_t result = _mkgmtime(&t);
    return (result == (time_t)-1) ? 0 : result;
}

/* ── پارس یه instance از Set-Cookie ──
   فرمت: "name=value; Attr1=Val1; Attr2; Attr3=Val3" — طبق RFC 6265
   ولی با تحمل نسبت به سرورهای واقعی که همیشه دقیق نیستن (مثلاً
   فاصله‌ی اضافه، حروف بزرگ/کوچیک نامنظم در اسم attribute). فقط
   attribute هایی که واقعاً برامون مهمن رو تشخیص می‌ده: Domain،
   Path، Max-Age، Expires، Secure، HttpOnly — بقیه (SameSite و
   غیره) بی‌صدا نادیده گرفته می‌شن چون این یه client دانلوده، نه
   مرورگر، و این attribute ها فقط برای کنترل ارسال cross-site از
   یه browsing context معنا دارن. */
static void parse_set_cookie(const char *raw, const char *default_domain,
                              char *out_domain, int out_domain_sz,
                              char *out_path,   int out_path_sz,
                              char *out_name,   int out_name_sz,
                              char *out_value,  int out_value_sz,
                              time_t *out_expires,
                              bool *out_secure, bool *out_http_only) {
    out_domain[0] = out_path[0] = out_name[0] = out_value[0] = '\0';
    *out_expires = 0;
    *out_secure = *out_http_only = false;

    bool have_max_age = false;

    char work[1024];
    safe_copy(work, sizeof(work), raw);

    /* توکنایز دستی روی ';' — به‌جای strtok_s (که در بعضی
       cross-toolchain های mingw-w64، بسته به اینکه به msvcrt یا
       UCRT لینک بشن، ممکنه اصلاً declare نشده باشه). این نسخه‌ی
       دستی به هیچ CRT خاصی وابسته نیست و رفتارش کاملاً پیش‌بینی‌پذیره. */
    bool first = true;
    char *p = work;
    while (p && *p) {
        char *semi = strchr(p, ';');
        if (semi) *semi = '\0';
        char *tok = p;
        p = semi ? semi + 1 : NULL;
        /* trim فضای پیشوندی */
        while (*tok == ' ' || *tok == '\t') tok++;

        if (first) {
            /* اولین token همیشه name=value خود کوکیه */
            char *eq = strchr(tok, '=');
            if (eq) {
                int nlen = (int)(eq - tok);
                if (nlen >= out_name_sz) nlen = out_name_sz - 1;
                memcpy(out_name, tok, nlen);
                out_name[nlen] = '\0';
                safe_copy(out_value, out_value_sz, eq + 1);
            }
            first = false;
        } else {
            char *eq = strchr(tok, '=');
            char attr_name[32] = {0};
            if (eq) {
                int nlen = (int)(eq - tok);
                if (nlen >= (int)sizeof(attr_name)) nlen = sizeof(attr_name) - 1;
                memcpy(attr_name, tok, nlen);
                attr_name[nlen] = '\0';
            } else {
                safe_copy(attr_name, sizeof(attr_name), tok);
            }
            /* trim فضای انتهایی اسم attribute */
            int alen = (int)strlen(attr_name);
            while (alen > 0 && (attr_name[alen-1] == ' ' || attr_name[alen-1] == '\t'))
                attr_name[--alen] = '\0';

            const char *val = eq ? eq + 1 : "";

            if (_stricmp(attr_name, "Domain") == 0 && val[0]) {
                safe_copy(out_domain, out_domain_sz, val);
            } else if (_stricmp(attr_name, "Path") == 0 && val[0]) {
                safe_copy(out_path, out_path_sz, val);
            } else if (_stricmp(attr_name, "Max-Age") == 0 && val[0]) {
                long long age = _atoi64(val);
                *out_expires = (age <= 0) ? (time_t)(time(NULL) - 1)
                                          : (time_t)(time(NULL) + age);
                have_max_age = true;
            } else if (_stricmp(attr_name, "Expires") == 0 && val[0] && !have_max_age) {
                /* Max-Age (بالا) اولویت داره طبق RFC 6265 §5.3 —
                   اگه هر دو باشن Expires نادیده گرفته میشه. */
                time_t parsed = parse_http_date(val);
                if (parsed != 0) *out_expires = parsed;
            } else if (_stricmp(attr_name, "Secure") == 0) {
                *out_secure = true;
            } else if (_stricmp(attr_name, "HttpOnly") == 0) {
                *out_http_only = true;
            }
            /* SameSite و attribute های ناشناخته: عمداً نادیده */
        }
    }

    /* اگه Domain نبود، host-only (بدون leading dot) با همون
       domain ای که این پاسخ ازش اومده — دقیقاً معادل رفتار مرورگر
       برای کوکی بدون Domain attribute. */
    if (!out_domain[0]) {
        safe_copy(out_domain, out_domain_sz, default_domain);
    } else {
        /* Domain صریح یعنی subdomain match هم باید کار کنه —
           پیشوند '.' همون قرارداد leading-dot این فایله (رجوع کن
           به domain_matches). یه '.' پیشوندی که سرور خودش گذاشته
           (فرم قدیمی‌تر ولی هنوز رایج) رو دوباره اضافه نمی‌کنیم. */
        if (out_domain[0] != '.') {
            char tmp[CJ_DOMAIN_LEN];
            snprintf(tmp, sizeof(tmp), ".%s", out_domain);
            safe_copy(out_domain, out_domain_sz, tmp);
        }
    }
    if (!out_path[0]) strcpy(out_path, "/");
}

static void hook_on_set_cookie(IDM_Download *dl, const char *host,
                               const char *raw_set_cookie) {
    (void)dl;
    if (!g_cj_plugin_instance || !host || !raw_set_cookie) return;
    CJ_Priv *priv = (CJ_Priv *)g_cj_plugin_instance->priv;

    char domain[CJ_DOMAIN_LEN], path[CJ_PATH_LEN];
    char name[CJ_NAME_LEN], value[CJ_VALUE_LEN];
    time_t expires; bool secure, http_only;

    parse_set_cookie(raw_set_cookie, host,
                     domain, sizeof(domain), path, sizeof(path),
                     name, sizeof(name), value, sizeof(value),
                     &expires, &secure, &http_only);

    if (!name[0]) return; /* هدر بدشکل بود — چیزی برای ذخیره نیست */

    cj_set_full(priv, domain, path, name, value, expires, secure, http_only);

    /* ذخیره‌ی فوری (نه فقط موقع خروج تمیز برنامه) — یه session
       cookie که همین الان از سرور یاد گرفتیم، اگه برنامه قبل از
       خروج تمیز crash کنه یا force-kill بشه، قبلاً کاملاً از دست
       می‌رفت. چون cj_save_file خودش با priv->dirty گارد شده،
       وقتی واقعاً چیزی عوض نشده این عملاً هیچ I/O اضافه‌ای نداره. */
    cj_save_file(priv);

    LOGI(LOG_MOD_COOKIE, "learned cookie '%s' for %s%s", name, domain, path);
}

static int hook_init(void) {
    if (!g_cj_plugin_instance) return -1;
    CJ_Priv *priv = (CJ_Priv *)g_cj_plugin_instance->priv;
    cj_load_file(priv);
    LOGI(LOG_MOD_COOKIE, "initialized (%d cookies)", priv->count);
    return 0;
}

static void hook_destroy(void) {
    if (!g_cj_plugin_instance) return;
    CJ_Priv *priv = (CJ_Priv *)g_cj_plugin_instance->priv;
    cj_save_file(priv);
    LOGI(LOG_MOD_COOKIE, "destroyed");
}

/* ======================================================
   Public API
   ====================================================== */
IDM_PluginHooks *cookie_jar_plugin_create(const char *cookie_file) {
    CJ_Priv *priv = (CJ_Priv *)calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (cookie_file) {
        safe_copy(priv->cookie_file, IDM_MAX_PATH, cookie_file);
    } else {
        char appdata[IDM_MAX_PATH] = {0};
        #define CJ_SUFFIX      "\\IDM-Clone\\cookies.txt"
        #define CJ_SUFFIX_LEN  22
        if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            int ap_max = IDM_MAX_PATH - CJ_SUFFIX_LEN - 2;
            snprintf(priv->cookie_file, IDM_MAX_PATH, "%.*s%s",
                     ap_max, appdata, CJ_SUFFIX);
        } else {
            strcpy(priv->cookie_file, ".\\cookies.txt");
        }
    }

    InitializeCriticalSection(&priv->lock);

    IDM_PluginHooks *plugin = (IDM_PluginHooks *)calloc(1, sizeof(*plugin));
    if (!plugin) { free(priv); return NULL; }

    plugin->name             = "cookie_jar";
    plugin->version          = "1.0";
    plugin->api_version      = IDM_PLUGIN_API_VERSION;
    plugin->priv             = priv;
    plugin->on_init          = hook_init;
    plugin->on_destroy       = hook_destroy;
    plugin->on_http_headers  = hook_on_http_headers;
    plugin->on_set_cookie    = hook_on_set_cookie;

    g_cj_plugin_instance = plugin;
    return plugin;
}

void cookie_jar_plugin_destroy(IDM_PluginHooks *plugin) {
    if (!plugin) return;
    CJ_Priv *priv = (CJ_Priv *)plugin->priv;
    if (priv) {
        DeleteCriticalSection(&priv->lock);
        free(priv);
    }
    if (g_cj_plugin_instance == plugin)
        g_cj_plugin_instance = NULL;
    free(plugin);
}

void cookie_jar_set(IDM_PluginHooks *p, const char *domain,
                    const char *name, const char *value, int max_age_sec) {
    if (!p) return;
    cj_set_internal((CJ_Priv *)p->priv, domain, name, value, max_age_sec);
}

void cookie_jar_import(IDM_PluginHooks *p, const char *netscape_file) {
    if (!p || !netscape_file) return;
    CJ_Priv *priv = (CJ_Priv *)p->priv;
    char old_file[IDM_MAX_PATH];
    safe_copy(old_file, IDM_MAX_PATH, priv->cookie_file);
    safe_copy(priv->cookie_file, IDM_MAX_PATH, netscape_file);
    cj_load_file(priv);
    safe_copy(priv->cookie_file, IDM_MAX_PATH, old_file);
}

void cookie_jar_save(IDM_PluginHooks *p) {
    if (!p) return;
    CJ_Priv *priv = (CJ_Priv *)p->priv;
    priv->dirty = true;
    cj_save_file(priv);
}

void cookie_jar_clear(IDM_PluginHooks *p) {
    if (!p) return;
    CJ_Priv *priv = (CJ_Priv *)p->priv;
    EnterCriticalSection(&priv->lock);
    memset(priv->cookies, 0, sizeof(priv->cookies));
    priv->count = 0;
    priv->dirty = true;
    LeaveCriticalSection(&priv->lock);
}
