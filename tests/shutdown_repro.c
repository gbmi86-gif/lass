/* shutdown_repro.c — اثبات عملی فیکس ترتیب shutdown_brake.
   این فایل جزو build اصلی نیست؛ جدا کامپایل و اجرا میشه.

   شبیه‌سازی می‌کنه:
   1) ترتیب قدیمی (باگ‌دار): wait_all قبل از سیگنال واقعی توقف
   2) ترتیب جدید (فیکس‌شده): core_signal_shutdown قبل از wait_all

   برای هر دو، زمان سپری‌شده‌ی wait_all و نتیجه‌ی برگشتی‌اش رو چاپ
   می‌کنه. انتظار: حالت (1) همیشه ~5000ms کامل رو طی کنه و FALSE
   برگردونه (timeout)؛ حالت (2) خیلی سریع (چند صد ms یا کمتر) و
   TRUE برگردونه.
*/
#include <windows.h>
#include <stdio.h>
#include "../core/core_ldm.h"
#include "../src/shutdown_brake.h"

static double ms_since(LARGE_INTEGER start, LARGE_INTEGER freq) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

int main(void) {
    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);

    printf("=== Scenario A: OLD order (wait_all BEFORE any real stop-signal) ===\n");
    {
        LDM_Core *core = core_create(NULL);
        if (!core) { printf("core_create failed\n"); return 1; }
        Sleep(300); /* بذار پلاگین‌ها/threadهای پس‌زمینه واقعاً بالا بیان */

        shutdown_brake_begin_drain();
        QueryPerformanceCounter(&t0);
        BOOL all_done = shutdown_brake_wait_all(5000);
        double elapsed = ms_since(t0, freq);
        int still_active = shutdown_brake_active_count();

        printf("  wait_all() returned %s after %.0f ms, still_active=%d\n",
               all_done ? "TRUE" : "FALSE", elapsed, still_active);

        /* حالا واقعاً سیگنال بده و آزاد کن (تا leak نکنیم) */
        core_signal_shutdown(core);
        shutdown_brake_wait_all(5000);
        core_destroy(core);
        shutdown_brake_reset_for_tests();
    }

    printf("\n=== Scenario B: NEW order (core_signal_shutdown BEFORE wait_all) ===\n");
    {
        LDM_Core *core = core_create(NULL);
        if (!core) { printf("core_create failed\n"); return 1; }
        Sleep(300);

        core_signal_shutdown(core);
        shutdown_brake_begin_drain();
        QueryPerformanceCounter(&t0);
        BOOL all_done = shutdown_brake_wait_all(5000);
        double elapsed = ms_since(t0, freq);
        int still_active = shutdown_brake_active_count();

        printf("  wait_all() returned %s after %.0f ms, still_active=%d\n",
               all_done ? "TRUE" : "FALSE", elapsed, still_active);

        core_destroy(core);
        shutdown_brake_reset_for_tests();
    }

    return 0;
}
