/* checksum.c — پیاده‌سازی با CryptoAPI (advapi32) */
#include "checksum.h"
#include <wincrypt.h>
#include <stdio.h>

const char *checksum_algo_name(ChecksumAlgo algo) {
    switch (algo) {
    case CHECKSUM_MD5:    return "MD5";
    case CHECKSUM_SHA1:   return "SHA-1";
    case CHECKSUM_SHA256: return "SHA-256";
    default:              return "?";
    }
}

/* اندازه‌ی chunk خوندن از دیسک — عمداً نسبتاً بزرگ (256KB) تا
   تعداد syscall/CryptHashData برای فایل‌های بزرگ (چند گیگابایتی)
   منطقی بمونه، ولی هنوز حافظه‌ی ناچیزی مصرف کنه. */
#define CHECKSUM_CHUNK_SIZE (256 * 1024)

BOOL checksum_compute_file(const char *path, ChecksumAlgo algo,
                            char out_hex[CHECKSUM_HEX_MAXLEN]) {
    if (!path || !out_hex) return FALSE;
    out_hex[0] = '\0';

    ALG_ID alg_id;
    switch (algo) {
    case CHECKSUM_MD5:    alg_id = CALG_MD5;    break;
    case CHECKSUM_SHA1:   alg_id = CALG_SHA1;   break;
    case CHECKSUM_SHA256: alg_id = CALG_SHA_256; break;
    default: return FALSE;
    }

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BOOL ok = FALSE;

    /* PROV_RSA_AES پشتیبانی می‌کنه از MD5/SHA1/SHA256 هر سه —
       CRYPT_VERIFYCONTEXT چون فقط hashing لازمه، نه key persist
       شده‌ای که نیاز به keyset داشته باشه. */
    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_AES,
                               CRYPT_VERIFYCONTEXT)) {
        CloseHandle(hFile);
        return FALSE;
    }

    if (!CryptCreateHash(hProv, alg_id, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
        return FALSE;
    }

    BYTE *buf = (BYTE *)malloc(CHECKSUM_CHUNK_SIZE);
    if (!buf) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
        return FALSE;
    }

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(hFile, buf, CHECKSUM_CHUNK_SIZE, &read, NULL)) {
            goto done;
        }
        if (read == 0) break; /* EOF */
        if (!CryptHashData(hHash, buf, read, 0)) {
            goto done;
        }
    }

    {
        BYTE digest[32]; /* بزرگ‌ترین حالت SHA-256 = 32 بایت */
        DWORD digest_len = sizeof(digest);
        if (!CryptGetHashParam(hHash, HP_HASHVAL, digest, &digest_len, 0))
            goto done;

        for (DWORD i = 0; i < digest_len; i++)
            snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
        out_hex[digest_len * 2] = '\0';
        ok = TRUE;
    }

done:
    free(buf);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    CloseHandle(hFile);
    return ok;
}
