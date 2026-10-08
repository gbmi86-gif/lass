/* secure_store.c — پیاده‌سازی رمزنگاری با DPAPI
   ─────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include "secure_store.h"
#include "lass_log.h"

#define DPAPI_PREFIX "DPAPI:"
#define DPAPI_PREFIX_LEN 6

/* ── Base64 ساده (بدون کتابخانه‌ی خارجی) ──
   چون DPAPI باینری برمی‌گردونه ولی INI/Registry فقط رشته‌ی
   ASCII می‌پذیرن، باید base64 encode/decode داشته باشیم. */
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const BYTE *data, DWORD len, char *out, int out_sz) {
    int oi = 0;
    for (DWORD i = 0; i < len; i += 3) {
        if (oi + 4 >= out_sz) return -1; /* buffer کافی نیست */
        DWORD n = data[i];
        n <<= 8; if (i+1 < len) n |= data[i+1];
        n <<= 8; if (i+2 < len) n |= data[i+2];

        out[oi++] = B64_CHARS[(n >> 18) & 0x3F];
        out[oi++] = B64_CHARS[(n >> 12) & 0x3F];
        out[oi++] = (i+1 < len) ? B64_CHARS[(n >> 6) & 0x3F] : '=';
        out[oi++] = (i+2 < len) ? B64_CHARS[n & 0x3F]        : '=';
    }
    if (oi >= out_sz) return -1;
    out[oi] = '\0';
    return oi;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, BYTE *out, int out_sz) {
    int oi = 0;
    int len = (int)strlen(in);
    for (int i = 0; i < len; i += 4) {
        int v[4] = {0,0,0,0};
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            if (i+j >= len || in[i+j] == '=') { pad++; v[j] = 0; continue; }
            int d = b64_decode_char(in[i+j]);
            if (d < 0) return -1; /* کاراکتر نامعتبر — رد کن */
            v[j] = d;
        }
        if (oi >= out_sz) return -1;
        out[oi++] = (BYTE)((v[0] << 2) | (v[1] >> 4));
        if (pad < 2) {
            if (oi >= out_sz) return -1;
            out[oi++] = (BYTE)((v[1] << 4) | (v[2] >> 2));
        }
        if (pad < 1) {
            if (oi >= out_sz) return -1;
            out[oi++] = (BYTE)((v[2] << 6) | v[3]);
        }
    }
    return oi;
}

BOOL secure_store_encrypt(const char *plaintext, char *out_b64, int out_sz) {
    if (!plaintext || !out_b64 || out_sz <= 0) return FALSE;
    out_b64[0] = '\0';

    if (plaintext[0] == '\0') return TRUE; /* رشته‌ی خالی نیازی به رمزنگاری نداره */

    DATA_BLOB in_blob, out_blob;
    in_blob.pbData = (BYTE *)plaintext;
    in_blob.cbData = (DWORD)strlen(plaintext);

    /* CRYPTPROTECT_UI_FORBIDDEN: هیچ‌وقت دیالوگ ویندوز نشون نده
       (این یه عملیات background هست، نباید کاربر رو معطل کنه) */
    if (!CryptProtectData(&in_blob, L"Lass Download Manager",
                           NULL, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        LOGE("secure_store", "CryptProtectData failed (err=%lu)",
             (unsigned long)GetLastError());
        return FALSE;
    }

    /* پیشوند + base64 */
    int avail = out_sz - DPAPI_PREFIX_LEN - 1;
    if (avail <= 0) { LocalFree(out_blob.pbData); return FALSE; }

    memcpy(out_b64, DPAPI_PREFIX, DPAPI_PREFIX_LEN);
    int enc_len = b64_encode(out_blob.pbData, out_blob.cbData,
                              out_b64 + DPAPI_PREFIX_LEN, avail);
    LocalFree(out_blob.pbData);

    if (enc_len < 0) {
        LOGE("secure_store", "Encrypted data too large for buffer (%d bytes available)", out_sz);
        out_b64[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

BOOL secure_store_decrypt(const char *in_b64_or_plain, char *out_plain, int out_sz) {
    if (!in_b64_or_plain || !out_plain || out_sz <= 0) return FALSE;
    out_plain[0] = '\0';

    if (in_b64_or_plain[0] == '\0') return TRUE;

    /* اگه پیشوند DPAPI نداره، یعنی plaintext قدیمیه (از نسخه‌های
       قبل از این فیچر) — همون رو برگردون بدون تغییر. این یعنی
       upgrade بدون از دست دادن تنظیمات کار می‌کنه. */
    if (strncmp(in_b64_or_plain, DPAPI_PREFIX, DPAPI_PREFIX_LEN) != 0) {
        strncpy(out_plain, in_b64_or_plain, out_sz - 1);
        out_plain[out_sz - 1] = '\0';
        return TRUE;
    }

    const char *b64_part = in_b64_or_plain + DPAPI_PREFIX_LEN;
    BYTE decoded[2048];
    int decoded_len = b64_decode(b64_part, decoded, sizeof(decoded));
    if (decoded_len < 0) {
        LOGE("secure_store", "Base64 decode failed for encrypted value");
        return FALSE;
    }

    DATA_BLOB in_blob, out_blob;
    in_blob.pbData = decoded;
    in_blob.cbData = (DWORD)decoded_len;

    if (!CryptUnprotectData(&in_blob, NULL, NULL, NULL, NULL,
                             CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        /* این می‌تونه به‌خاطر user profile متفاوت یا کپی شدن فایل
           به ماشین دیگه باشه — طبیعی و امنه که fail بشه (این
           دقیقاً همون محافظتیه که DPAPI ارائه میده) */
        LOGW("secure_store", "CryptUnprotectData failed (err=%lu) — "
             "value may be from a different user/machine",
             (unsigned long)GetLastError());
        return FALSE;
    }

    DWORD copy_len = out_blob.cbData;
    if (copy_len >= (DWORD)out_sz) copy_len = (DWORD)out_sz - 1;
    memcpy(out_plain, out_blob.pbData, copy_len);
    out_plain[copy_len] = '\0';
    LocalFree(out_blob.pbData);
    return TRUE;
}
