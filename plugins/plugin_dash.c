/* plugin_dash.c
   ─────────────────────────────────────────────────────────────
   DASH / MPD: تجزیه‌ی manifest و ساخت representation ها.

   یک DOM کوچک ساخته می‌شود: هر تگ یک node با والد، فرزند و برادر بعدی.
   attribute ها با کوتیشن و entity درست خوانده می‌شوند (نه با strstr).
   همه‌ی آدرس‌ها مطلق می‌شوند (BaseURL در هر سطح). فقط manifest های
   پشتیبانی‌شده ساخته می‌شوند؛ بقیه با دلیل صریح رد می‌شوند.
   قالب segment با یک ساختار ثابت گسترش می‌یابد؛ هیچ format string از
   manifest به printf داده نمی‌شود.
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "security_fixes.h"
#include "plugin_dash.h"
#include "../src/lass_log.h"
#include "../src/types.h"

#define DASH_HTTP_MAX            (8u * 1024u * 1024u)
#define DASH_MAX_NODES           200000
#define DASH_MAX_DEPTH           64
#define DASH_MAX_TOTAL_SEGMENTS  32768

/* ======================================================
   DOM کوچک XML
   ====================================================== */
typedef struct {
    const char *name;        /* نام محلی (بدون پیشوند namespace) */
    int         name_len;
    const char *attrs;       /* متن بین نام و پایان تگ */
    int         attrs_len;
    const char *text;        /* متن بین این تگ و تگ بعدی */
    int         text_len;
    int         parent;
    int         first_child;
    int         last_child;
    int         next_sibling;
} XNode;

typedef struct {
    XNode *nodes;
    int    count;
    int    cap;
} XDoc;

static void set_unsupported(char *dst, size_t sz, const char *why) {
    if (dst[0] == '\0') safe_strncpy(dst, why, sz);
}

static int xdoc_push(XDoc *d) {
    XNode *n;
    int idx;
    if (d->count >= DASH_MAX_NODES) return -1;
    if (d->count == d->cap) {
        int nc = d->cap ? d->cap * 2 : 256;
        XNode *a;
        if (nc > DASH_MAX_NODES) nc = DASH_MAX_NODES;
        a = (XNode *)realloc(d->nodes, (size_t)nc * sizeof(XNode));
        if (!a) return -1;
        d->nodes = a;
        d->cap = nc;
    }
    idx = d->count++;
    n = &d->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->parent = -1;
    n->first_child = -1;
    n->last_child = -1;
    n->next_sibling = -1;
    return idx;
}

static void local_name(const char *s, size_t from, size_t to, const char **out, int *out_len) {
    size_t p = from, i;
    for (i = from; i < to; i++) if (s[i] == ':') p = i + 1;
    *out = s + p;
    *out_len = (int)(to - p);
}

static BOOL name_is(const XNode *n, const char *name) {
    size_t l = strlen(name);
    return (size_t)n->name_len == l && memcmp(n->name, name, l) == 0;
}

static size_t find_seq(const char *s, size_t len, size_t from, const char *pat) {
    size_t pl = strlen(pat), i;
    for (i = from; i + pl <= len; i++) {
        if (memcmp(s + i, pat, pl) == 0) return i;
    }
    return SIZE_MAX;
}

/* entity های استاندارد XML؛ هر چیز دیگر همان‌طور می‌ماند */
static void xml_decode(const char *src, size_t n, char *out, size_t out_sz) {
    size_t o = 0, i = 0;
    if (out_sz == 0) return;
    while (i < n && o + 1 < out_sz) {
        if (src[i] == '&') {
            size_t e = i + 1;
            while (e < n && e - i <= 6 && src[e] != ';') e++;
            if (e < n && src[e] == ';') {
                size_t l = e - (i + 1);
                char c = 0;
                if (l == 3 && memcmp(src + i + 1, "amp", 3) == 0) c = '&';
                else if (l == 2 && memcmp(src + i + 1, "lt", 2) == 0) c = '<';
                else if (l == 2 && memcmp(src + i + 1, "gt", 2) == 0) c = '>';
                else if (l == 4 && memcmp(src + i + 1, "quot", 4) == 0) c = '"';
                else if (l == 4 && memcmp(src + i + 1, "apos", 4) == 0) c = '\'';
                if (c) {
                    out[o++] = c;
                    i = e + 1;
                    continue;
                }
            }
        }
        out[o++] = src[i++];
    }
    out[o] = '\0';
}

/* مقدار یک attribute؛ کوتیشن ' یا " هر دو پذیرفته است */
static BOOL node_attr(const XNode *n, const char *name, char *out, size_t out_sz) {
    const char *p = n->attrs, *end = n->attrs + n->attrs_len;
    size_t nl = strlen(name);
    out[0] = '\0';
    while (p < end) {
        const char *ns;
        size_t an;
        char q;
        const char *vs;
        size_t vl;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        ns = p;
        while (p < end && *p != '=' && !isspace((unsigned char)*p)) p++;
        an = (size_t)(p - ns);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '=') break;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || (*p != '"' && *p != '\'')) break;
        q = *p++;
        vs = p;
        while (p < end && *p != q) p++;
        if (p >= end) break;
        vl = (size_t)(p - vs);
        p++;
        if (an == nl && memcmp(ns, name, nl) == 0) {
            xml_decode(vs, vl, out, out_sz);
            return TRUE;
        }
    }
    return FALSE;
}

static void text_value(const XNode *n, char *out, size_t out_sz) {
    const char *a = n->text, *b = n->text + n->text_len;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    xml_decode(a, (size_t)(b - a), out, out_sz);
}

static int child_named(const XDoc *d, int parent, const char *name) {
    int c;
    if (parent < 0) return -1;
    for (c = d->nodes[parent].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        if (name_is(&d->nodes[c], name)) return c;
    }
    return -1;
}

static int next_named(const XDoc *d, int idx, const char *name) {
    int c;
    for (c = d->nodes[idx].next_sibling; c >= 0; c = d->nodes[c].next_sibling) {
        if (name_is(&d->nodes[c], name)) return c;
    }
    return -1;
}

/* tokenizer: تگ‌های شروع، پایان، خودبسته؛ کامنت، CDATA، declaration */
static BOOL xml_parse(const char *s, size_t len, XDoc *d) {
    size_t i = 0;
    int stack[DASH_MAX_DEPTH];
    int sp = 0;
    while (i < len) {
        if (s[i] != '<') {
            i++;
            continue;
        }
        if (i + 4 <= len && memcmp(s + i, "<!--", 4) == 0) {
            size_t e = find_seq(s, len, i + 4, "-->");
            if (e == SIZE_MAX) return FALSE;
            i = e + 3;
            continue;
        }
        if (i + 9 <= len && memcmp(s + i, "<![CDATA[", 9) == 0) {
            size_t e = find_seq(s, len, i + 9, "]]>");
            if (e == SIZE_MAX) return FALSE;
            i = e + 3;
            continue;
        }
        if (i + 1 < len && (s[i + 1] == '?' || s[i + 1] == '!')) {
            size_t e = i + 2;
            while (e < len && s[e] != '>') e++;
            if (e >= len) return FALSE;
            i = e + 1;
            continue;
        }
        if (i + 1 < len && s[i + 1] == '/') {
            size_t ns = i + 2, j = ns, e;
            const char *ln;
            int lnl;
            int k;
            while (j < len && s[j] != '>' && !isspace((unsigned char)s[j])) j++;
            local_name(s, ns, j, &ln, &lnl);
            e = j;
            while (e < len && s[e] != '>') e++;
            if (e >= len) return FALSE;
            for (k = sp - 1; k >= 0; k--) {
                const XNode *on = &d->nodes[stack[k]];
                if (on->name_len == lnl && memcmp(on->name, ln, (size_t)lnl) == 0) {
                    sp = k;
                    break;
                }
            }
            i = e + 1;
            continue;
        }
        {
            size_t ns = i + 1, j = ns, e, ae;
            const char *ln;
            int lnl;
            BOOL selfclose = FALSE;
            char q = 0;
            int idx, parent;
            XNode *n;
            size_t ts, te;
            while (j < len && s[j] != '>' && s[j] != '/' && !isspace((unsigned char)s[j])) j++;
            if (j == ns) return FALSE;
            local_name(s, ns, j, &ln, &lnl);
            e = j;
            while (e < len) {
                char c = s[e];
                if (q) {
                    if (c == q) q = 0;
                } else if (c == '"' || c == '\'') {
                    q = c;
                } else if (c == '>') {
                    break;
                }
                e++;
            }
            if (e >= len) return FALSE;
            ae = e;
            {
                size_t k = e;
                while (k > j && isspace((unsigned char)s[k - 1])) k--;
                if (k > j && s[k - 1] == '/') {
                    selfclose = TRUE;
                    ae = k - 1;
                }
            }
            idx = xdoc_push(d);
            if (idx < 0) return FALSE;
            parent = sp > 0 ? stack[sp - 1] : -1;
            n = &d->nodes[idx];
            n->name = ln;
            n->name_len = lnl;
            n->attrs = s + j;
            n->attrs_len = (int)(ae - j);
            n->parent = parent;
            if (parent >= 0) {
                XNode *p = &d->nodes[parent];
                if (p->first_child < 0) p->first_child = idx;
                else d->nodes[p->last_child].next_sibling = idx;
                p->last_child = idx;
            }
            ts = e + 1;
            te = ts;
            while (te < len && s[te] != '<') te++;
            n->text = s + ts;
            n->text_len = (int)(te - ts);
            if (!selfclose) {
                if (sp >= DASH_MAX_DEPTH) return FALSE;
                stack[sp++] = idx;
            }
            i = e + 1;
        }
    }
    return TRUE;
}

/* ======================================================
   URL و BaseURL
   ====================================================== */
static void url_join(const char *base, const char *rel, char *out, size_t out_sz) {
    char b[DASH_MAX_URL];
    size_t rl;
    out[0] = '\0';
    if (out_sz == 0) return;
    if (!rel || !*rel) {
        safe_strncpy(out, base ? base : "", out_sz);
        return;
    }
    if (_strnicmp(rel, "http://", 7) == 0 || _strnicmp(rel, "https://", 8) == 0) {
        safe_strncpy(out, rel, out_sz);
        return;
    }
    if (!base) {
        safe_strncpy(out, rel, out_sz);
        return;
    }
    rl = strlen(rel);
    safe_strncpy(b, base, sizeof(b));
    {
        char *q = strchr(b, '?');
        if (q) *q = '\0';
    }
    if (rel[0] == '/' && rel[1] == '/') {
        const char *se = strstr(b, "://");
        size_t sl;
        if (!se) {
            safe_strncpy(out, rel, out_sz);
            return;
        }
        sl = (size_t)(se - b) + 1u;           /* شامل ':' */
        if (sl + rl + 1u > out_sz) return;
        memcpy(out, b, sl);
        memcpy(out + sl, rel, rl + 1u);
        return;
    }
    if (rel[0] == '/') {
        const char *se = strstr(b, "://");
        const char *he;
        size_t plen;
        if (!se) {
            safe_strncpy(out, rel, out_sz);
            return;
        }
        he = strchr(se + 3, '/');
        plen = he ? (size_t)(he - b) : strlen(b);
        if (plen + rl + 1u > out_sz) return;
        memcpy(out, b, plen);
        memcpy(out + plen, rel, rl + 1u);
        return;
    }
    {
        const char *last = strrchr(b, '/');
        const char *se = strstr(b, "://");
        size_t dlen;
        BOOL host_only;
        if (!last && !se) {
            safe_strncpy(out, rel, out_sz);
            return;
        }
        host_only = (!last) || (se && last < se + 3);
        dlen = host_only ? strlen(b) : (size_t)(last - b) + 1u;
        if (dlen + rl + 2u > out_sz) return;
        memcpy(out, b, dlen);
        if (host_only) out[dlen++] = '/';
        memcpy(out + dlen, rel, rl + 1u);
    }
}

/* base یک node: زنجیره‌ی BaseURL از MPD تا خود node، هر کدام نسبت به قبلی */
static void base_of(const XDoc *d, int idx, const char *mpd_url, char *out, size_t out_sz) {
    int chain[DASH_MAX_DEPTH];
    int n = 0, k;
    char cur[DASH_MAX_URL], nxt[DASH_MAX_URL];
    for (k = idx; k >= 0 && n < DASH_MAX_DEPTH; k = d->nodes[k].parent) chain[n++] = k;
    safe_strncpy(cur, mpd_url ? mpd_url : "", sizeof(cur));
    for (k = n - 1; k >= 0; k--) {
        int bu = child_named(d, chain[k], "BaseURL");
        if (bu >= 0) {
            char t[DASH_MAX_URL];
            text_value(&d->nodes[bu], t, sizeof(t));
            if (t[0]) {
                url_join(cur, t, nxt, sizeof(nxt));
                if (nxt[0]) safe_strncpy(cur, nxt, sizeof(cur));
            }
        }
    }
    safe_strncpy(out, cur, out_sz);
}

/* ======================================================
   مدت ISO 8601 (PT…S، P…D، PT…H…M…S)
   ====================================================== */
static BOOL parse_iso_duration(const char *s, double *out) {
    const char *p = s;
    double total = 0.0;
    BOOL in_time = FALSE, any = FALSE;
    if (!s || *s != 'P') return FALSE;
    p++;
    while (*p) {
        double num = 0.0, scale = 0.1;
        BOOL has = FALSE;
        if (*p == 'T') {
            in_time = TRUE;
            p++;
            continue;
        }
        while (isdigit((unsigned char)*p)) {
            num = num * 10.0 + (double)(*p - '0');
            has = TRUE;
            p++;
        }
        if (*p == '.' || *p == ',') {
            p++;
            while (isdigit((unsigned char)*p)) {
                num += (double)(*p - '0') * scale;
                scale *= 0.1;
                has = TRUE;
                p++;
            }
        }
        if (!has) return FALSE;
        switch (*p) {
        case 'D': if (in_time) return FALSE; total += num * 86400.0; break;
        case 'H': if (!in_time) return FALSE; total += num * 3600.0; break;
        case 'M': if (!in_time) return FALSE; total += num * 60.0; break;
        case 'S': if (!in_time) return FALSE; total += num; break;
        default:  return FALSE;   /* Y، W و ماه در بخش تاریخ پشتیبانی نمی‌شوند */
        }
        p++;
        any = TRUE;
    }
    if (!any) return FALSE;
    *out = total;
    return TRUE;
}

/* ======================================================
   گسترش قالب segment
   $RepresentationID$ ، $Bandwidth$ ، $Number$ ، $Number%0Nd$ ، $$
   هر متغیر دیگر (از جمله $Time$) با FALSE رد می‌شود.
   ====================================================== */
static BOOL expand_template(const char *tmpl, const char *rep_id, long long bandwidth,
                            BOOL have_number, long long number, char *out, size_t out_sz) {
    size_t o = 0;
    const char *p = tmpl;
    if (!tmpl || out_sz == 0) return FALSE;
    out[0] = '\0';
    while (*p) {
        const char *e;
        size_t nl;
        char piece[128];
        size_t pl;
        if (*p != '$') {
            if (o + 1 >= out_sz) return FALSE;
            out[o++] = *p++;
            continue;
        }
        if (p[1] == '$') {
            if (o + 1 >= out_sz) return FALSE;
            out[o++] = '$';
            p += 2;
            continue;
        }
        e = strchr(p + 1, '$');
        if (!e) return FALSE;
        nl = (size_t)(e - (p + 1));
        if (nl == 16 && memcmp(p + 1, "RepresentationID", 16) == 0) {
            snprintf(piece, sizeof(piece), "%s", rep_id ? rep_id : "");
        } else if (nl == 9 && memcmp(p + 1, "Bandwidth", 9) == 0) {
            snprintf(piece, sizeof(piece), "%lld", bandwidth);
        } else if (nl >= 6 && memcmp(p + 1, "Number", 6) == 0) {
            if (!have_number) return FALSE;
            if (nl == 6) {
                snprintf(piece, sizeof(piece), "%lld", number);
            } else {
                /* فقط %0Nd با N رقمی؛ عرض از یک عدد اعتبارسنجی‌شده ساخته می‌شود */
                const char *f = p + 1 + 6;
                size_t fl = nl - 6;
                size_t digits, k;
                int width = 0;
                if (fl < 4 || f[0] != '%' || f[1] != '0' || f[fl - 1] != 'd') return FALSE;
                digits = fl - 3;
                if (digits > 2) return FALSE;
                for (k = 0; k < digits; k++) {
                    if (!isdigit((unsigned char)f[2 + k])) return FALSE;
                    width = width * 10 + (f[2 + k] - '0');
                }
                if (width < 1 || width > 9) return FALSE;
                snprintf(piece, sizeof(piece), "%0*lld", width, number);
            }
        } else {
            return FALSE;
        }
        pl = strlen(piece);
        if (o + pl + 1u > out_sz) return FALSE;
        memcpy(out + o, piece, pl);
        o += pl;
        p = e + 1;
    }
    out[o] = '\0';
    return TRUE;
}

/* ======================================================
   representation ها
   ====================================================== */
typedef enum { KIND_SKIP = 0, KIND_VIDEO, KIND_AUDIO } RepKind;

static BOOL contains_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    const char *p;
    for (p = hay; *p; p++) {
        if (_strnicmp(p, needle, nl) == 0) return TRUE;
    }
    return FALSE;
}

static RepKind rep_kind(const char *mime, const char *ctype, const char *codecs, BOOL has_width) {
    if (ctype[0]) {
        if (_stricmp(ctype, "video") == 0) return KIND_VIDEO;
        if (_stricmp(ctype, "audio") == 0) return KIND_AUDIO;
    }
    if (_strnicmp(mime, "video", 5) == 0) return KIND_VIDEO;
    if (_strnicmp(mime, "audio", 5) == 0) return KIND_AUDIO;
    if (_strnicmp(codecs, "mp4a", 4) == 0 || _strnicmp(codecs, "ac-3", 4) == 0 ||
        _strnicmp(codecs, "ec-3", 4) == 0 || _strnicmp(codecs, "opus", 4) == 0)
        return KIND_AUDIO;
    if (_strnicmp(codecs, "avc", 3) == 0 || _strnicmp(codecs, "hev", 3) == 0 ||
        _strnicmp(codecs, "hvc", 3) == 0 || _strnicmp(codecs, "vp0", 3) == 0 ||
        _strnicmp(codecs, "vp9", 3) == 0 || _strnicmp(codecs, "av01", 4) == 0)
        return KIND_VIDEO;
    if (has_width) return KIND_VIDEO;
    return KIND_SKIP;
}

/* مقدار attribute از خود node، اگر نبود از node دوم (مثلاً AdaptationSet) */
static BOOL attr_inh(const XDoc *d, int rep, int set, const char *name, char *out, size_t out_sz) {
    if (node_attr(&d->nodes[rep], name, out, out_sz)) return TRUE;
    if (set >= 0 && node_attr(&d->nodes[set], name, out, out_sz)) return TRUE;
    out[0] = '\0';
    return FALSE;
}

/* یک representation را پر می‌کند؛ اگر پشتیبانی نشود FALSE و دلیل در why */
static BOOL fill_rep(const XDoc *d, int rep, int set, const char *mpd_url,
                     const DASH_Manifest *mf, DASH_Representation *r,
                     long long *entries_used, char *why, size_t why_sz) {
    char tmp[DASH_MAX_URL], base[DASH_MAX_URL], num[32];
    int st, sl, sb;
    memset(r, 0, sizeof(*r));
    why[0] = '\0';
    node_attr(&d->nodes[rep], "id", r->id, sizeof(r->id));
    if (attr_inh(d, rep, set, "bandwidth", num, sizeof(num)))
        r->bandwidth = (int)strtol(num, NULL, 10);
    if (attr_inh(d, rep, set, "width", num, sizeof(num)))
        r->width = (int)strtol(num, NULL, 10);
    if (attr_inh(d, rep, set, "height", num, sizeof(num)))
        r->height = (int)strtol(num, NULL, 10);
    attr_inh(d, rep, set, "codecs", r->codecs, sizeof(r->codecs));
    attr_inh(d, rep, set, "mimeType", r->mime_type, sizeof(r->mime_type));
    base_of(d, rep, mpd_url, base, sizeof(base));

    st = child_named(d, rep, "SegmentTemplate");
    if (st < 0 && set >= 0) st = child_named(d, set, "SegmentTemplate");
    sl = child_named(d, rep, "SegmentList");
    if (sl < 0 && set >= 0) sl = child_named(d, set, "SegmentList");
    sb = child_named(d, rep, "SegmentBase");
    if (sb < 0 && set >= 0) sb = child_named(d, set, "SegmentBase");

    if (st >= 0) {
        const XNode *tn = &d->nodes[st];
        long long timescale = 1, duration = 0, start = 1;
        double count_d;
        char media[DASH_MAX_URL];
        if (child_named(d, st, "SegmentTimeline") >= 0) {
            snprintf(why, why_sz, "SegmentTimeline is not supported yet");
            return FALSE;
        }
        if (!node_attr(tn, "media", media, sizeof(media))) {
            snprintf(why, why_sz, "SegmentTemplate without a media attribute");
            return FALSE;
        }
        if (strstr(media, "$Time$")) {
            snprintf(why, why_sz, "SegmentTemplate with $Time$ is not supported yet");
            return FALSE;
        }
        if (node_attr(tn, "timescale", num, sizeof(num))) timescale = strtoll(num, NULL, 10);
        if (node_attr(tn, "duration", num, sizeof(num))) duration = strtoll(num, NULL, 10);
        if (node_attr(tn, "startNumber", num, sizeof(num))) start = strtoll(num, NULL, 10);
        if (timescale <= 0 || duration <= 0) {
            snprintf(why, why_sz, "SegmentTemplate without a valid duration");
            return FALSE;
        }
        if (!expand_template(media, r->id, r->bandwidth, TRUE, start, tmp, sizeof(tmp))) {
            snprintf(why, why_sz, "SegmentTemplate media uses an unsupported variable");
            return FALSE;
        }
        if (mf->total_duration <= 0.0) {
            snprintf(why, why_sz, "the manifest has no total duration");
            return FALSE;
        }
        /* تلورانس کوچک: 0.48*12800 گاهی 6144.000000001 می‌شود */
        count_d = ceil(mf->total_duration * (double)timescale / (double)duration - 1e-6);
        if (count_d < 1.0 || count_d > (double)DASH_MAX_SEGMENTS) {
            snprintf(why, why_sz, "the number of segments is out of range");
            return FALSE;
        }
        url_join(base, media, r->media_template, sizeof(r->media_template));
        if (node_attr(tn, "initialization", tmp, sizeof(tmp))) {
            char init_exp[DASH_MAX_URL];
            if (!expand_template(tmp, r->id, r->bandwidth, FALSE, 0, init_exp, sizeof(init_exp))) {
                snprintf(why, why_sz, "SegmentTemplate initialization uses an unsupported variable");
                return FALSE;
            }
            url_join(base, init_exp, r->init_url, sizeof(r->init_url));
        }
        r->use_template = TRUE;
        r->start_number = start;
        r->timescale = timescale;
        r->duration = duration;
        r->segment_count = (long long)count_d;
        return TRUE;
    }

    if (sl >= 0) {
        int ini = child_named(d, sl, "Initialization");
        int c, n = 0, k = 0;
        if (ini < 0 || !node_attr(&d->nodes[ini], "sourceURL", tmp, sizeof(tmp))) {
            snprintf(why, why_sz, "SegmentList without an Initialization sourceURL");
            return FALSE;
        }
        url_join(base, tmp, r->init_url, sizeof(r->init_url));
        for (c = child_named(d, sl, "SegmentURL"); c >= 0; c = next_named(d, c, "SegmentURL")) {
            if (node_attr(&d->nodes[c], "mediaRange", tmp, sizeof(tmp)) ||
                node_attr(&d->nodes[c], "indexRange", tmp, sizeof(tmp))) {
                snprintf(why, why_sz, "byte-range segments are not supported yet");
                return FALSE;
            }
            n++;
        }
        if (n == 0) {
            snprintf(why, why_sz, "SegmentList without segments");
            return FALSE;
        }
        if (n > DASH_MAX_SEGMENTS || *entries_used + n > DASH_MAX_TOTAL_SEGMENTS) {
            snprintf(why, why_sz, "the manifest has too many segment addresses");
            return FALSE;
        }
        r->segments = (char (*)[DASH_MAX_URL])calloc((size_t)n, DASH_MAX_URL);
        if (!r->segments) {
            snprintf(why, why_sz, "out of memory");
            return FALSE;
        }
        for (c = child_named(d, sl, "SegmentURL"); c >= 0; c = next_named(d, c, "SegmentURL")) {
            if (!node_attr(&d->nodes[c], "media", tmp, sizeof(tmp))) {
                free(r->segments);
                r->segments = NULL;
                snprintf(why, why_sz, "SegmentURL without a media attribute");
                return FALSE;
            }
            url_join(base, tmp, r->segments[k], DASH_MAX_URL);
            k++;
        }
        r->segment_list_count = n;
        r->use_template = FALSE;
        *entries_used += n;
        return TRUE;
    }

    if (sb >= 0) {
        snprintf(why, why_sz, "SegmentBase (single file with index) is not supported yet");
        return FALSE;
    }
    snprintf(why, why_sz, "representation %s has no segment information", r->id);
    return FALSE;
}

/* ======================================================
   ساخت manifest از DOM
   ====================================================== */
static BOOL build_manifest(const XDoc *d, const char *mpd_url, DASH_Manifest *mf) {
    int root = -1, period0 = -1, periods = 0, a;
    char tmp[DASH_MAX_URL];
    long long entries = 0;
    int i;

    for (i = 0; i < d->count; i++) {
        if (name_is(&d->nodes[i], "MPD")) {
            root = i;
            break;
        }
    }
    if (root < 0) return FALSE;

    if (node_attr(&d->nodes[root], "type", tmp, sizeof(tmp)) && strcmp(tmp, "dynamic") == 0) {
        mf->is_live = TRUE;
        set_unsupported(mf->unsupported, sizeof(mf->unsupported),
                        "live (dynamic) manifests are not supported");
    }
    if (node_attr(&d->nodes[root], "mediaPresentationDuration", tmp, sizeof(tmp))) {
        double v = 0.0;
        if (parse_iso_duration(tmp, &v)) mf->total_duration = v;
    }
    for (i = 0; i < d->count; i++) {
        if (name_is(&d->nodes[i], "ContentProtection")) {
            set_unsupported(mf->unsupported, sizeof(mf->unsupported),
                            "protected (DRM) content is not supported");
            break;
        }
    }

    {
        int c;
        for (c = child_named(d, root, "Period"); c >= 0; c = next_named(d, c, "Period")) {
            if (period0 < 0) period0 = c;
            periods++;
        }
    }
    if (periods == 0) {
        set_unsupported(mf->unsupported, sizeof(mf->unsupported), "the manifest has no Period");
        return TRUE;
    }
    if (periods > 1) {
        set_unsupported(mf->unsupported, sizeof(mf->unsupported),
                        "manifests with more than one Period are not supported yet");
    }
    if (mf->total_duration <= 0.0 && node_attr(&d->nodes[period0], "duration", tmp, sizeof(tmp))) {
        double v = 0.0;
        if (parse_iso_duration(tmp, &v)) mf->total_duration = v;
    }

    for (a = child_named(d, period0, "AdaptationSet"); a >= 0; a = next_named(d, a, "AdaptationSet")) {
        int rp;
        for (rp = child_named(d, a, "Representation"); rp >= 0; rp = next_named(d, rp, "Representation")) {
            char mime[64] = "", ctype[32] = "", codecs[128] = "", why[DASH_MAX_UNSUPPORTED] = "";
            char wbuf[16];
            BOOL has_w;
            RepKind kind;
            attr_inh(d, rp, a, "mimeType", mime, sizeof(mime));
            attr_inh(d, rp, a, "contentType", ctype, sizeof(ctype));
            attr_inh(d, rp, a, "codecs", codecs, sizeof(codecs));
            has_w = attr_inh(d, rp, a, "width", wbuf, sizeof(wbuf));
            kind = rep_kind(mime, ctype, codecs, has_w);
            if (kind == KIND_SKIP) continue;
            if (contains_ci(mime, "webm") || contains_ci(codecs, "vp8") ||
                contains_ci(codecs, "vorbis")) {
                if (mf->reject_reason[0] == '\0')
                    safe_strncpy(mf->reject_reason, "WebM segments are not supported yet",
                                 sizeof(mf->reject_reason));
                continue;
            }
            if (kind == KIND_VIDEO) {
                DASH_Representation *r;
                if (mf->video_count >= DASH_MAX_REPRESENTATIONS) continue;
                r = &mf->video[mf->video_count];
                if (!fill_rep(d, rp, a, mpd_url, mf, r, &entries, why, sizeof(why))) {
                    set_unsupported(mf->reject_reason, sizeof(mf->reject_reason), why);
                    continue;
                }
                mf->video_count++;
            } else {
                DASH_Representation *r;
                if (mf->audio_count >= DASH_MAX_REPRESENTATIONS) continue;
                r = &mf->audio[mf->audio_count];
                if (!fill_rep(d, rp, a, mpd_url, mf, r, &entries, why, sizeof(why))) {
                    set_unsupported(mf->reject_reason, sizeof(mf->reject_reason), why);
                    continue;
                }
                mf->audio_count++;
            }
        }
    }

    if (mf->unsupported[0] == '\0' && mf->video_count + mf->audio_count == 0) {
        if (mf->reject_reason[0] != '\0')
            safe_strncpy(mf->unsupported, mf->reject_reason, sizeof(mf->unsupported));
        else
            safe_strncpy(mf->unsupported, "no supported representation found", sizeof(mf->unsupported));
    }

    mf->best_video = 0;
    for (i = 1; i < mf->video_count; i++) {
        if (mf->video[i].bandwidth > mf->video[mf->best_video].bandwidth) mf->best_video = i;
    }
    mf->best_audio = 0;
    for (i = 1; i < mf->audio_count; i++) {
        if (mf->audio[i].bandwidth > mf->audio[mf->best_audio].bandwidth) mf->best_audio = i;
    }
    return TRUE;
}

BOOL dash_parse_manifest(const char *mpd_url, const char *text, size_t len, DASH_Manifest *mf) {
    XDoc d;
    BOOL ok;
    if (!mf || !text) return FALSE;
    memset(mf, 0, sizeof(*mf));
    memset(&d, 0, sizeof(d));
    safe_strncpy(mf->base_url, mpd_url ? mpd_url : "", sizeof(mf->base_url));
    if (!xml_parse(text, len, &d)) {
        free(d.nodes);
        return FALSE;
    }
    ok = build_manifest(&d, mpd_url ? mpd_url : "", mf);
    free(d.nodes);
    return ok;
}

void dash_manifest_free(DASH_Manifest *mf) {
    int i;
    if (!mf) return;
    for (i = 0; i < mf->video_count; i++) {
        free(mf->video[i].segments);
        mf->video[i].segments = NULL;
    }
    for (i = 0; i < mf->audio_count; i++) {
        free(mf->audio[i].segments);
        mf->audio[i].segments = NULL;
    }
}

uint32_t dash_rep_segment_count(const DASH_Representation *r) {
    if (!r) return 0u;
    if (r->use_template) return r->segment_count > 0 ? (uint32_t)r->segment_count : 0u;
    return r->segment_list_count > 0 ? (uint32_t)r->segment_list_count : 0u;
}

BOOL dash_rep_segment_url(const DASH_Representation *r, uint32_t index, char *out, size_t out_sz) {
    if (!r || !out || out_sz == 0) return FALSE;
    out[0] = '\0';
    if (r->use_template) {
        if ((long long)index >= r->segment_count) return FALSE;
        return expand_template(r->media_template, r->id, r->bandwidth, TRUE,
                               r->start_number + (long long)index, out, out_sz);
    }
    if ((int)index >= r->segment_list_count || !r->segments) return FALSE;
    if (r->segments[index][0] == '\0') return FALSE;
    safe_strncpy(out, r->segments[index], out_sz);
    return TRUE;
}

/* ======================================================
   HTTP (برای مسیر نمایش کیفیت در sniffer؛ دانلود کامل در dash_download.c)
   ====================================================== */
static BOOL dash_http_get(const char *url, char **out, size_t *out_len) {
    HINTERNET hi, hu;
    DWORD code = 0, cl = sizeof(code), idx = 0;
    char *buf;
    size_t cap = 64u * 1024u, n = 0;
    *out = NULL;
    *out_len = 0;
    hi = InternetOpenA("LassDM-DASH", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hi) return FALSE;
    hu = InternetOpenUrlA(hi, url, NULL, 0,
                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!hu) {
        InternetCloseHandle(hi);
        return FALSE;
    }
    if (!HttpQueryInfoA(hu, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &cl, &idx) ||
        code >= 400) {
        InternetCloseHandle(hu);
        InternetCloseHandle(hi);
        return FALSE;
    }
    buf = (char *)malloc(cap);
    if (!buf) {
        InternetCloseHandle(hu);
        InternetCloseHandle(hi);
        return FALSE;
    }
    for (;;) {
        DWORD got = 0;
        if (n == cap) {
            size_t nc;
            char *nb;
            if (cap >= DASH_HTTP_MAX) {
                free(buf);
                InternetCloseHandle(hu);
                InternetCloseHandle(hi);
                return FALSE;   /* بزرگ‌تر از سقف: بی‌صدا کوتاه نمی‌شود */
            }
            nc = cap * 2u;
            if (nc > DASH_HTTP_MAX) nc = DASH_HTTP_MAX;
            nb = (char *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                InternetCloseHandle(hu);
                InternetCloseHandle(hi);
                return FALSE;
            }
            buf = nb;
            cap = nc;
        }
        if (!InternetReadFile(hu, buf + n, (DWORD)(cap - n), &got)) {
            free(buf);
            InternetCloseHandle(hu);
            InternetCloseHandle(hi);
            return FALSE;
        }
        if (got == 0) break;
        n += got;
    }
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    if (n == 0) {
        free(buf);
        return FALSE;
    }
    *out = buf;
    *out_len = n;
    return TRUE;
}

IDM_Result dash_fetch_manifest(const char *url, DASH_Manifest *mf) {
    char *buf = NULL;
    size_t len = 0;
    if (!url || !mf) return IDM_ERR_INVALID;
    memset(mf, 0, sizeof(*mf));
    if (!dash_http_get(url, &buf, &len)) return IDM_ERR_NETWORK;
    if (!dash_parse_manifest(url, buf, len, mf)) {
        free(buf);
        dash_manifest_free(mf);
        return IDM_ERR_CORRUPT;
    }
    free(buf);
    return IDM_OK;
}

/* ======================================================
   تشخیص URL و content
   ====================================================== */
BOOL dash_is_mpd_url(const char *url) {
    const char *q;
    int base_len;
    if (!url) return FALSE;
    q = strchr(url, '?');
    base_len = q ? (int)(q - url) : (int)strlen(url);
    if (base_len >= 4 && _strnicmp(url + base_len - 4, ".mpd", 4) == 0) return TRUE;
    if (q && strstr(q, "mpd")) return TRUE;
    return FALSE;
}

BOOL dash_is_mpd_content(const char *text, int len) {
    if (!text || len < 4) return FALSE;
    return (strstr(text, "<MPD") != NULL);
}

/* ======================================================
   Plugin lifecycle
   ====================================================== */
static int  dash_on_init   (void) { return 0; }
static void dash_on_destroy(void) {}

IDM_PluginHooks *dash_plugin_create(void) {
    IDM_PluginHooks *plug = (IDM_PluginHooks *)calloc(1, sizeof(*plug));
    if (!plug) return NULL;
    plug->name        = "dash_mpd";
    plug->version     = "1.0";
    plug->api_version = IDM_PLUGIN_API_VERSION;
    plug->on_init     = dash_on_init;
    plug->on_destroy  = dash_on_destroy;
    LOGI(LOG_MOD_DASH, "plugin created");
    return plug;
}

void dash_plugin_destroy(IDM_PluginHooks *plug) {
    if (!plug) return;
    free(plug);
}
