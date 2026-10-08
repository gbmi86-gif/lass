/* crash_report.c — رجوع کن به crash_report.h */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crash_report.h"
#include "lass_log.h"

#define CR_MAX_FRAMES 24

/* «نام ماژول+offset» برای یه آدرس. precision روی %s سقف طول رو
   مشخص می‌کنه، پس خروجی همیشه در بافر ۵۱۲ بایتی جا می‌شه. */
static void describe_addr(char *out, size_t sz, DWORD64 addr) {
    HMODULE mod = NULL;
    if (addr &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &mod) && mod) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(mod, path, sizeof(path) - 1);
        const char *base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        snprintf(out, sz, "%.200s+0x%llX", base,
                 (unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
    } else {
        snprintf(out, sz, "<unknown module> 0x%llX", (unsigned long long)addr);
    }
}

int crash_report_log(EXCEPTION_POINTERS *ep) {
    if (!ep || !ep->ExceptionRecord) return 0;
    char buf[512];

    describe_addr(buf, sizeof(buf),
                  (DWORD64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    LOGE(LOG_MOD_CORE, "Fault location: %s", buf);

    int frames = 0;
#if defined(_M_X64) || defined(__x86_64__)
    if (!ep->ContextRecord) return 0;
    CONTEXT ctx = *ep->ContextRecord;

    LOGE(LOG_MOD_CORE, "Call stack (most recent call first):");
    for (int i = 0; i < CR_MAX_FRAMES; i++) {
        DWORD64 pc = ctx.Rip;
        describe_addr(buf, sizeof(buf), pc);
        LOGE(LOG_MOD_CORE, "  #%02d %s", i, buf);
        frames++;
        if (pc == 0 && i > 0) break;

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &image_base, NULL);
        if (rf) {
            PVOID    hdata = NULL;
            DWORD64  frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, rf, &ctx,
                             &hdata, &frame, NULL);
        } else {
            /* تابع leaf (یا صدا زدن از طریق اشاره‌گر NULL، که pc=0 میشه):
               آدرس برگشت دقیقاً بالای stack هست. */
            DWORD64 ret = 0;
            SIZE_T  got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(),
                                   (LPCVOID)(uintptr_t)ctx.Rsp,
                                   &ret, sizeof(ret), &got) ||
                got != sizeof(ret))
                break;
            ctx.Rip = ret;
            ctx.Rsp += 8;
        }
        if (ctx.Rip == 0) break;
    }
#endif
    return frames;
}
