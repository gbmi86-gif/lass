/* url_secure.c — رجوع کن به url_secure.h */

#include <windows.h>
#include <string.h>
#include "url_secure.h"

/* پایان بخش authority در URL (و در متن‌های JSON/لاگ) */
static BOOL is_authority_end(char c) {
    return c == '\0' || c == '/' || c == '?' || c == '#' || c == '"' || c == '\'' ||
           c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
           c == '\\' || c == '}' || c == ']' || c == ')';
}

static BOOL copy_span(char *dst, size_t dst_sz, const char *src, size_t len) {
    if (len + 1 > dst_sz) return FALSE;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return TRUE;
}

/* اولین authority بعد از "://" را پیدا می‌کند؛ end و آخرین '@' داخل آن را برمی‌گرداند */
static const char *find_authority(const char *in, const char **auth_end, const char **at) {
    const char *sch = strstr(in, "://");
    if (!sch) return NULL;
    const char *auth = sch + 3;
    const char *end = auth;
    while (!is_authority_end(*end)) end++;
    const char *last_at = NULL;
    for (const char *q = auth; q < end; q++)
        if (*q == '@') last_at = q;
    *auth_end = end;
    *at = last_at;
    return auth;
}

BOOL url_split_credentials(const char *in, char *clean_out, size_t clean_sz,
                           char *pass_out, size_t pass_sz) {
    if (!in || !clean_out || clean_sz == 0 || !pass_out || pass_sz == 0) return FALSE;
    pass_out[0] = '\0';
    const char *auth_end = NULL, *at = NULL;
    const char *auth = find_authority(in, &auth_end, &at);
    if (!auth || !at) return copy_span(clean_out, clean_sz, in, strlen(in));

    const char *colon = NULL;
    for (const char *q = auth; q < at; q++)
        if (*q == ':') { colon = q; break; }
    if (!colon) return copy_span(clean_out, clean_sz, in, strlen(in));

    size_t plen = (size_t)(at - colon - 1);
    if (plen >= pass_sz) return FALSE;
    memcpy(pass_out, colon + 1, plen);
    pass_out[plen] = '\0';

    size_t prefix = (size_t)(colon - in);
    size_t tail = strlen(at);
    if (prefix + tail + 1 > clean_sz) return FALSE;
    memcpy(clean_out, in, prefix);
    memcpy(clean_out + prefix, at, tail + 1);
    return TRUE;
}

BOOL url_inject_password(const char *clean, const char *pass, char *out, size_t out_sz) {
    if (!clean || !pass || !out || out_sz == 0) return FALSE;
    const char *auth_end = NULL, *at = NULL;
    const char *auth = find_authority(clean, &auth_end, &at);
    if (!auth || !at) return FALSE;
    for (const char *q = auth; q < at; q++)
        if (*q == ':') return FALSE;            /* رمز از قبل وجود دارد */

    size_t prefix = (size_t)(at - clean);
    size_t plen   = strlen(pass);
    size_t tail   = strlen(at);
    if (prefix + 1 + plen + tail + 1 > out_sz) return FALSE;
    memcpy(out, clean, prefix);
    out[prefix] = ':';
    memcpy(out + prefix + 1, pass, plen);
    memcpy(out + prefix + 1 + plen, at, tail + 1);
    return TRUE;
}

void url_redact_for_log(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t w = 0;
    const char *p = in;
    while (*p && w + 1 < out_sz) {
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
            /* کپی "://" */
            out[w++] = *p++;
            out[w++] = *p++;
            out[w++] = *p++;
            if (w + 1 >= out_sz) break;

            const char *auth = p;
            const char *end = auth;
            while (!is_authority_end(*end)) end++;
            const char *at = NULL;
            for (const char *q = auth; q < end; q++)
                if (*q == '@') at = q;
            const char *colon = NULL;
            if (at) {
                for (const char *q = auth; q < at; q++)
                    if (*q == ':') { colon = q; break; }
            }
            if (colon) {
                size_t ul = (size_t)(colon - auth);
                if (w + ul + 4 + 1 > out_sz) break;
                memcpy(out + w, auth, ul);
                w += ul;
                memcpy(out + w, ":***", 4);
                w += 4;
                p = at;                          /* '@' و بقیه‌ی URL دست‌نخورده کپی می‌شود */
            }
            continue;
        }
        out[w++] = *p++;
    }
    out[w] = '\0';
}
