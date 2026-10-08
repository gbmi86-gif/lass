/* shutdown_brake.c — پیاده‌سازی ترمز خروج ناگهانی
   ─────────────────────────────────────────────────
   طراحی امن (مطابق فلسفه‌ی پروژه):
   - یه آرایه‌ی fixed-size (نه لیست دینامیک) — بدون malloc در
     مسیر hot، بدون ریسک heap fragmentation یا leak.
   - همه‌ی دسترسی‌ها زیر یه CRITICAL_SECTION واحد — این ماژول
     خودش نباید تبدیل به یه منبع جدید race condition بشه.
   - shutdown_brake_wait_all با polling کوتاه (نه event-based
     پیچیده) پیاده‌سازی شده — ساده‌تر، کمتر مستعد خطا، و چون این
     تابع فقط یک‌بار در کل عمر برنامه (هنگام خروج) صدا زده میشه،
     overhead پولینگ کاملاً بی‌اهمیته.
   ─────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "shutdown_brake.h"
#include "lass_log.h"

typedef struct {
    BOOL   in_use;
    char   name[SHUTDOWN_BRAKE_NAME_LEN];
    DWORD  registered_at;   /* GetTickCount هنگام register */
    int    order;           /* ترتیب ثبت‌نام — برای LIFO logging */
} BrakeEntry;

static struct {
    CRITICAL_SECTION lock;
    BOOL             lock_ready;
    BrakeEntry       entries[SHUTDOWN_BRAKE_MAX_ENTRIES];
    volatile BOOL    draining;
    int              next_order;
} g_brake;

static void ensure_lock_init(void) {
    /* الگوی lazy-init امن — مشابه چیزی که در browser_ext برای
       g_log_lock استفاده شد، با double-checked pattern درست */
    static volatile LONG init_state = 0; /* 0=not-init, 1=in-progress, 2=ready */
    if (init_state == 2) return;
    if (InterlockedCompareExchange(&init_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_brake.lock);
        MemoryBarrier();
        InterlockedExchange(&init_state, 2);
    } else {
        while (init_state < 2) Sleep(0);
    }
}

int shutdown_brake_register(const char *name) {
    ensure_lock_init();
    EnterCriticalSection(&g_brake.lock);

    if (g_brake.draining) {
        LeaveCriticalSection(&g_brake.lock);
        LOGW("shutdown_brake",
             "Registration rejected for '%s': app is draining/shutting down",
             name ? name : "?");
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < SHUTDOWN_BRAKE_MAX_ENTRIES; i++) {
        if (!g_brake.entries[i].in_use) { slot = i; break; }
    }

    if (slot < 0) {
        /* آرایه پره — این نباید در عمل اتفاق بیفته (۶۴ subsystem
           همزمان خیلی زیاده)، ولی اگه افتاد، بهتره ثبت‌نام رد بشه
           تا یه overflow خاموش داشته باشیم، نه crash */
        LeaveCriticalSection(&g_brake.lock);
        LOGE("shutdown_brake",
             "Registry full (%d entries) — cannot register '%s'",
             SHUTDOWN_BRAKE_MAX_ENTRIES, name ? name : "?");
        return -1;
    }

    g_brake.entries[slot].in_use = TRUE;
    strncpy(g_brake.entries[slot].name, name ? name : "unnamed",
            SHUTDOWN_BRAKE_NAME_LEN - 1);
    g_brake.entries[slot].name[SHUTDOWN_BRAKE_NAME_LEN - 1] = '\0';
    g_brake.entries[slot].registered_at = GetTickCount();
    g_brake.entries[slot].order = g_brake.next_order++;

    int order = g_brake.entries[slot].order;
    LeaveCriticalSection(&g_brake.lock);

    LOGD("shutdown_brake", "Registered '%s' (slot=%d, order=%d)",
         name ? name : "?", slot, order);
    return slot;
}

void shutdown_brake_unregister(int id) {
    if (id < 0 || id >= SHUTDOWN_BRAKE_MAX_ENTRIES) return;
    ensure_lock_init();

    EnterCriticalSection(&g_brake.lock);
    if (g_brake.entries[id].in_use) {
        DWORD alive_ms = GetTickCount() - g_brake.entries[id].registered_at;
        LOGD("shutdown_brake", "Unregistered '%s' (was alive %lu ms)",
             g_brake.entries[id].name, (unsigned long)alive_ms);
        g_brake.entries[id].in_use = FALSE;
        g_brake.entries[id].name[0] = '\0';
    }
    LeaveCriticalSection(&g_brake.lock);
}

void shutdown_brake_begin_drain(void) {
    ensure_lock_init();
    EnterCriticalSection(&g_brake.lock);
    g_brake.draining = TRUE;
    int active = 0;
    for (int i = 0; i < SHUTDOWN_BRAKE_MAX_ENTRIES; i++)
        if (g_brake.entries[i].in_use) active++;
    LeaveCriticalSection(&g_brake.lock);

    LOGI("shutdown_brake",
         "Draining started — %d active subsystem(s), no new registrations allowed",
         active);
}

int shutdown_brake_active_count(void) {
    ensure_lock_init();
    EnterCriticalSection(&g_brake.lock);
    int active = 0;
    for (int i = 0; i < SHUTDOWN_BRAKE_MAX_ENTRIES; i++)
        if (g_brake.entries[i].in_use) active++;
    LeaveCriticalSection(&g_brake.lock);
    return active;
}

BOOL shutdown_brake_wait_all(DWORD timeout_ms) {
    ensure_lock_init();

    /* اگه draining شروع نشده بود، خودمون شروع می‌کنیم — این
       تابع باید idempotent باشه حتی اگه begin_drain فراموش بشه */
    if (!g_brake.draining) shutdown_brake_begin_drain();

    DWORD start = GetTickCount();
    for (;;) {
        int active = shutdown_brake_active_count();
        if (active == 0) {
            LOGI("shutdown_brake", "All subsystems shut down cleanly (took %lu ms)",
                 (unsigned long)(GetTickCount() - start));
            return TRUE;
        }

        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) {
            /* timeout — لیست کامل subsystem های باقی‌مونده رو با
               ترتیب LIFO (آخرین ثبت‌شده اول) لاگ کن. این دقیقاً
               همون چیزیه که کاربر خواسته بود: مشخص کردن اینکه
               کدوم قسمت مانع خروج تمیز شده. */
            LOGW("shutdown_brake",
                 "TIMEOUT after %lu ms — %d subsystem(s) still active:",
                 (unsigned long)elapsed, active);

            EnterCriticalSection(&g_brake.lock);
            /* مرتب‌سازی ساده بر اساس order، نزولی (LIFO) — چون
               تعداد entries کمه (حداکثر ۶۴)، یه O(n^2) ساده کافیه
               و نیازی به qsort اضافه نیست */
            int order_list[SHUTDOWN_BRAKE_MAX_ENTRIES];
            int idx_list[SHUTDOWN_BRAKE_MAX_ENTRIES];
            int n = 0;
            for (int i = 0; i < SHUTDOWN_BRAKE_MAX_ENTRIES; i++) {
                if (g_brake.entries[i].in_use) {
                    order_list[n] = g_brake.entries[i].order;
                    idx_list[n] = i;
                    n++;
                }
            }
            /* bubble sort نزولی بر اساس order — ساده و کافی برای n<=64 */
            for (int a = 0; a < n - 1; a++) {
                for (int b = 0; b < n - 1 - a; b++) {
                    if (order_list[b] < order_list[b+1]) {
                        int to = order_list[b]; order_list[b] = order_list[b+1]; order_list[b+1] = to;
                        int ti = idx_list[b];   idx_list[b]   = idx_list[b+1];   idx_list[b+1]   = ti;
                    }
                }
            }
            for (int k = 0; k < n; k++) {
                int i = idx_list[k];
                DWORD alive = GetTickCount() - g_brake.entries[i].registered_at;
                LOGW("shutdown_brake", "  [%d] '%s' — alive %lu ms (registered order=%d)",
                     k, g_brake.entries[i].name,
                     (unsigned long)alive, g_brake.entries[i].order);
            }
            LeaveCriticalSection(&g_brake.lock);

            return FALSE;
        }

        Sleep(50); /* polling کوتاه — این تابع فقط یک‌بار در خروج صدا زده میشه */
    }
}

void shutdown_brake_reset_for_tests(void) {
    ensure_lock_init();
    EnterCriticalSection(&g_brake.lock);
    memset(g_brake.entries, 0, sizeof(g_brake.entries));
    g_brake.draining = FALSE;
    g_brake.next_order = 0;
    LeaveCriticalSection(&g_brake.lock);
}
