#include "test_common.h"

int g_pass = 0;

int g_fail = 0;
int g_total = 0;

void test_sweep_stale_state(void) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*test*.idmstate", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileA(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
