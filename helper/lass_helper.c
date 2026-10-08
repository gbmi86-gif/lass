/* lass_helper.c
   ─────────────────────────────────────────────────────────────
   Lass Native Messaging Helper

   Chrome/Firefox این exe رو با stdin/stdout Native Messaging
   protocol اجرا می‌کنن. این helper پیام رو از stdin می‌خونه
   و به Named Pipe برنامه اصلی Lass می‌فرسته.

   Build:
     gcc -O2 -mwindows lass_helper.c -o lass_helper.exe -lkernel32

   نصب:
     lass_helper.exe --register   (ثبت در registry)
     lass_helper.exe --unregister (حذف از registry)
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_NAME  "\\\\.\\pipe\\LassNativeHost"
#define BUF_SIZE   (64 * 1024)

/* ── stdin به binary mode ── */
static void set_binary_mode(void) {
    /* از _setmode برای stdin/stdout استفاده نمی‌کنیم تا dependency نداشته باشیم
       به جاش از ReadFile / WriteFile مستقیم روی handle استفاده می‌کنیم */
}

/* ── یه پیام از pipe بخون (4 bytes length + payload) ── */
static char *pipe_read_msg(HANDLE pipe, uint32_t *out_len) {
    uint32_t len = 0;
    DWORD br = 0;
    if (!ReadFile(pipe, &len, 4, &br, NULL) || br != 4) return NULL;
    if (len == 0 || len > BUF_SIZE) return NULL;

    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;

    DWORD total = 0;
    while (total < len) {
        DWORD r = 0;
        if (!ReadFile(pipe, buf + total, len - total, &r, NULL) || r == 0) {
            free(buf); return NULL;
        }
        total += r;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ── یه پیام به pipe بنویس ── */
static BOOL pipe_write_msg(HANDLE pipe, const char *msg) {
    uint32_t len = (uint32_t)strlen(msg);
    DWORD written = 0;
    if (!WriteFile(pipe, &len, 4, &written, NULL) || written != 4) return FALSE;
    if (!WriteFile(pipe, msg, len, &written, NULL) || written != len) return FALSE;
    FlushFileBuffers(pipe);
    return TRUE;
}

/* ── stdin read (native messaging format) ── */
static char *stdin_read_msg(uint32_t *out_len) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    uint32_t len = 0;
    DWORD br = 0;
    if (!ReadFile(hStdin, &len, 4, &br, NULL) || br != 4) return NULL;
    if (len == 0 || len > BUF_SIZE) return NULL;

    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    DWORD total = 0;
    while (total < len) {
        DWORD r = 0;
        if (!ReadFile(hStdin, buf + total, len - total, &r, NULL) || r == 0) {
            free(buf); return NULL;
        }
        total += r;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ── stdout write ── */
static BOOL stdout_write_msg(const char *msg) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    uint32_t len = (uint32_t)strlen(msg);
    DWORD w = 0;
    if (!WriteFile(hOut, &len, 4, &w, NULL)) return FALSE;
    if (!WriteFile(hOut, msg, len, &w, NULL)) return FALSE;
    return TRUE;
}

/* ======================================================
   main: bridge stdin → pipe → stdout
   ====================================================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* ── اتصال به Named Pipe ── */
    HANDLE pipe = INVALID_HANDLE_VALUE;
    int retries = 5;
    while (retries-- > 0 && pipe == INVALID_HANDLE_VALUE) {
        pipe = CreateFileA(PIPE_NAME,
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeA(PIPE_NAME, 2000);
            } else {
                Sleep(200);
            }
        }
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        /* Lass در حال اجرا نیست — پیام خطا به extension */
        const char *err = "{\"status\":\"error\","
                          "\"message\":\"Lass is not running\"}";
        stdout_write_msg(err);
        return 1;
    }

    /* ── bridge loop ── */
    while (1) {
        /* خواندن از browser (stdin) */
        uint32_t msg_len = 0;
        char *msg = stdin_read_msg(&msg_len);
        if (!msg) break;

        /* ارسال به Lass (pipe) */
        BOOL ok = pipe_write_msg(pipe, msg);
        free(msg);
        if (!ok) break;

        /* دریافت response از Lass */
        char *resp = pipe_read_msg(pipe, NULL);
        if (!resp) break;

        /* ارسال به browser (stdout) */
        stdout_write_msg(resp);
        free(resp);
    }

    CloseHandle(pipe);
    return 0;
}
