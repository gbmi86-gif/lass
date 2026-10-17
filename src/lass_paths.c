/* lass_paths.c — رجوع کن به lass_paths.h */
#include <windows.h>
#include <shlobj.h>
#include <string.h>
#include "lass_paths.h"

#define LASS_ENV_DATA_DIR  "LASS_DATA_DIR"
#define LASS_PIPE_PROD     "\\\\.\\pipe\\LassNativeHost"
#define LASS_PIPE_TEST     "\\\\.\\pipe\\LassTestHost"
#define LASS_REG_SET_PROD  "Software\\Lass\\Settings"
#define LASS_REG_SET_TEST  "Software\\LassTest\\Settings"
#define LASS_RUN_PROD      "Lass Download Manager"
#define LASS_RUN_TEST      "Lass Download Manager (test)"

BOOL lass_paths_test_mode(void) {
    char probe[2] = {0};
    return GetEnvironmentVariableA(LASS_ENV_DATA_DIR, probe, (DWORD)sizeof(probe)) > 0;
}

BOOL lass_data_dir(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return FALSE;
    out[0] = '\0';

    char dir[MAX_PATH] = {0};
    if (lass_paths_test_mode()) {
        DWORD n = GetEnvironmentVariableA(LASS_ENV_DATA_DIR, dir, (DWORD)sizeof(dir));
        if (n == 0 || n >= sizeof(dir)) return FALSE;
    } else {
        char appdata[MAX_PATH] = {0};
        if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) return FALSE;
        const char *suf = "\\Lass";
        size_t al = strlen(appdata), sl = strlen(suf);
        if (al + sl + 1 > sizeof(dir)) return FALSE;
        memcpy(dir, appdata, al);
        memcpy(dir + al, suf, sl + 1);
    }

    size_t n = strlen(dir);
    if (n + 1 > out_sz) return FALSE;
    memcpy(out, dir, n + 1);
    return TRUE;
}

const char *lass_reg_settings_key(void) {
    return lass_paths_test_mode() ? LASS_REG_SET_TEST : LASS_REG_SET_PROD;
}

const char *lass_reg_startup_value(void) {
    return lass_paths_test_mode() ? LASS_RUN_TEST : LASS_RUN_PROD;
}

const char *lass_pipe_name(void) {
    return lass_paths_test_mode() ? LASS_PIPE_TEST : LASS_PIPE_PROD;
}

BOOL lass_reg_vendor_path(const char *prod_path, char *out, size_t out_sz) {
    if (!prod_path || !out || out_sz == 0) return FALSE;
    out[0] = '\0';
    if (!lass_paths_test_mode()) {
        size_t n = strlen(prod_path);
        if (n + 1 > out_sz) return FALSE;
        memcpy(out, prod_path, n + 1);
        return TRUE;
    }
    const char *prefix = "Software\\LassTest\\";
    const char *rest = (strncmp(prod_path, "Software\\", 9) == 0) ? prod_path + 9 : prod_path;
    size_t pl = strlen(prefix), rl = strlen(rest);
    if (pl + rl + 1 > out_sz) return FALSE;
    memcpy(out, prefix, pl);
    memcpy(out + pl, rest, rl + 1);
    return TRUE;
}

const char *lass_reg_browser_ext_key(void) {
    return lass_paths_test_mode() ? "Software\\LassTest\\Settings\\BrowserExtension"
                                  : "Software\\Lass\\Settings\\BrowserExtension";
}
