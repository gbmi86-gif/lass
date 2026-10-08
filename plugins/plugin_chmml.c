/* plugin_chmml.c
   ─────────────────────────────────────────────────────────────
   CHMML – Custom Heap Memory Manager Lock
   بازنویسی کامل برای MinGW64/Windows:
   - بدون pthread (از CRITICAL_SECTION ویندوز استفاده میکنه)
   - بدون VLA یا alloca
   - سازگار با MSVC و GCC
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "plugin_chmml.h"
#include "../src/lass_log.h"

/* ======================================================
   ثابت‌ها
   ====================================================== */
#define CHMML_MAGIC          0xDEADBEEFu
#define CHMML_MAGIC_FREE     0xFEEEFEEEu
#define CHMML_CANARY_PRE     0xABABABABABABABABull
#define CHMML_CANARY_POST    0xBABABABABABABABAull
#define CHMML_SLAB_CLASSES   8
#define CHMML_ALIGN          8
#define CHMML_REDZONE        8
#define CHMML_MAX_TRACKED    2048
#define CHMML_OBJS_PER_PAGE  64

/* اندازه‌های slab */
static const size_t slab_sizes[CHMML_SLAB_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* ======================================================
   Header هر allocation
   ====================================================== */
#pragma pack(push, 1)
typedef struct CHMML_Header {
    uint64_t           canary_pre;
    uint32_t           magic;
    uint32_t           user_size;    /* اندازه درخواستی کاربر */
    uint32_t           slab_class;   /* 0-7 = slab, 0xFFFF = large */
    uint32_t           alloc_id;
    const char        *file;
    int                line;
    const char        *func;
    struct CHMML_Header *next;       /* برای free list / large list */
    struct CHMML_Header *prev;
    uint64_t           canary_post;
} CHMML_Header;
#pragma pack(pop)

/* اندازه header align شده به ۸ */
#define HDR_SZ  ((sizeof(CHMML_Header) + 7u) & ~7u)

/* ======================================================
   Slab page
   ====================================================== */
typedef struct CHMML_SlabPage {
    struct CHMML_SlabPage *next;
    size_t                 obj_count;
    uint8_t                data[];   /* C99 flexible array */
} CHMML_SlabPage;

typedef struct {
    size_t          obj_size;
    size_t          block_size;  /* HDR_SZ + REDZONE + obj_size, aligned */
    size_t          free_count;
    size_t          total_count;
    CHMML_Header   *free_list;
    CHMML_SlabPage *pages;
} CHMML_Slab;

/* ======================================================
   Allocation record (برای leak tracking)
   ====================================================== */
typedef struct {
    void       *ptr;
    size_t      size;
    const char *file;
    int         line;
    const char *func;
    uint32_t    alloc_id;
    DWORD       timestamp;  /* GetTickCount() */
} CHMML_AllocInfo;

/* ======================================================
   Heap context
   ====================================================== */
typedef struct {
    CHMML_Slab      slabs[CHMML_SLAB_CLASSES];
    CHMML_Header   *large_list;
    CHMML_AllocInfo *track_table;
    int              track_cap;
    int              track_count;

    /* آمار */
    volatile LONGLONG bytes_used;
    volatile LONGLONG bytes_peak;
    volatile LONGLONG total_allocs;
    volatile LONGLONG total_frees;
    volatile LONGLONG slab_hits;
    volatile LONGLONG slab_misses;
    volatile LONGLONG overflows;
    volatile LONGLONG leaks;

    uint32_t         next_id;
    bool             enable_canary;
    bool             enable_leak_report;

    CRITICAL_SECTION lock;
    bool             initialized;
} CHMML_Heap;

/* تک heap global برای کل پروسه */
static CHMML_Heap g_heap;
static bool       g_heap_ready = false;

/* ======================================================
   ابزارها
   ====================================================== */
static inline size_t align_up(size_t n, size_t a) {
    return (n + a - 1) & ~(a - 1);
}

static int find_slab_class(size_t size) {
    for (int i = 0; i < CHMML_SLAB_CLASSES; i++)
        if (size <= slab_sizes[i]) return i;
    return -1;
}

/* ======================================================
   Leak tracking
   ====================================================== */
static void track_add(void *ptr, size_t size,
                       const char *file, int line, const char *func) {
    if (!g_heap.track_table || !ptr) return;
    for (int i = 0; i < g_heap.track_cap; i++) {
        if (g_heap.track_table[i].ptr == NULL) {
            g_heap.track_table[i].ptr       = ptr;
            g_heap.track_table[i].size      = size;
            g_heap.track_table[i].file      = file;
            g_heap.track_table[i].line      = line;
            g_heap.track_table[i].func      = func;
            g_heap.track_table[i].alloc_id  = g_heap.next_id++;
            g_heap.track_table[i].timestamp = GetTickCount();
            g_heap.track_count++;
            return;
        }
    }
    /* جدول پر است – بدون tracking */
}

static void track_remove(void *ptr) {
    if (!g_heap.track_table || !ptr) return;
    for (int i = 0; i < g_heap.track_cap; i++) {
        if (g_heap.track_table[i].ptr == ptr) {
            memset(&g_heap.track_table[i], 0, sizeof(CHMML_AllocInfo));
            g_heap.track_count--;
            return;
        }
    }
}

/* ======================================================
   Slab: رشد (اضافه کردن page جدید)
   ====================================================== */
static void slab_grow(int ci) {
    CHMML_Slab *slab = &g_heap.slabs[ci];
    size_t page_data  = CHMML_OBJS_PER_PAGE * slab->block_size;
    size_t page_total = sizeof(CHMML_SlabPage) + page_data;

    CHMML_SlabPage *page = (CHMML_SlabPage *)malloc(page_total);
    if (!page) return;

    page->next      = slab->pages;
    page->obj_count = CHMML_OBJS_PER_PAGE;
    slab->pages     = page;

    for (size_t i = 0; i < CHMML_OBJS_PER_PAGE; i++) {
        uint8_t       *block = page->data + i * slab->block_size;
        CHMML_Header  *h     = (CHMML_Header *)block;

        h->canary_pre  = CHMML_CANARY_PRE;
        h->magic       = CHMML_MAGIC_FREE;
        h->user_size   = (uint32_t)slab->obj_size;
        h->slab_class  = (uint32_t)ci;
        h->alloc_id    = 0;
        h->file        = NULL;
        h->line        = 0;
        h->func        = NULL;
        h->canary_post = CHMML_CANARY_POST;

        h->next        = slab->free_list;
        h->prev        = NULL;
        slab->free_list = h;
    }

    slab->free_count  += CHMML_OBJS_PER_PAGE;
    slab->total_count += CHMML_OBJS_PER_PAGE;
    InterlockedIncrement64(&g_heap.slab_misses);
}

/* ======================================================
   Slab: alloc
   ====================================================== */
static void *slab_alloc(int ci, size_t req_size,
                          const char *file, int line, const char *func) {
    CHMML_Slab *slab = &g_heap.slabs[ci];

    if (!slab->free_list) {
        slab_grow(ci);
        if (!slab->free_list) return NULL;
    }

    CHMML_Header *h = slab->free_list;
    slab->free_list  = h->next;
    slab->free_count--;

    h->magic      = CHMML_MAGIC;
    h->user_size  = (uint32_t)req_size;
    h->alloc_id   = g_heap.next_id++;
    h->file       = file;
    h->line       = line;
    h->func       = func;
    h->next       = NULL;
    h->prev       = NULL;

    uint8_t *user_ptr = (uint8_t *)h + HDR_SZ;

    /* redzone canary بعد از header */
    if (g_heap.enable_canary) {
        uint64_t *rz = (uint64_t *)user_ptr;
        *rz = CHMML_CANARY_PRE;
    }

    void *ret = user_ptr + CHMML_REDZONE;

    InterlockedIncrement64(&g_heap.slab_hits);
    InterlockedAdd64(&g_heap.bytes_used, (LONGLONG)slab->obj_size);
    InterlockedIncrement64(&g_heap.total_allocs);

    if (g_heap.bytes_used > g_heap.bytes_peak)
        InterlockedExchange64(&g_heap.bytes_peak, g_heap.bytes_used);

    if (file && func)
        track_add(ret, req_size, file, line, func);

    return ret;
}

/* ======================================================
   Slab: free
   ====================================================== */
static void slab_free(void *ptr, int ci) {
    CHMML_Slab   *slab = &g_heap.slabs[ci];
    CHMML_Header *h    = (CHMML_Header *)((uint8_t *)ptr
                          - CHMML_REDZONE - HDR_SZ);

    /* double-free check */
    if (h->magic == CHMML_MAGIC_FREE) {
        OutputDebugStringA("[CHMML] DOUBLE FREE detected!\n");
        return;
    }
    if (h->magic != CHMML_MAGIC) {
        OutputDebugStringA("[CHMML] INVALID PTR in free!\n");
        return;
    }

    /* canary check */
    if (g_heap.enable_canary) {
        uint64_t *rz = (uint64_t *)((uint8_t *)h + HDR_SZ);
        if (*rz != CHMML_CANARY_PRE) {
            InterlockedIncrement64(&g_heap.overflows);
            OutputDebugStringA("[CHMML] HEAP UNDERFLOW detected!\n");
        }
    }

    track_remove(ptr);

    /* پاک کردن محتوا برای کشف use-after-free */
    memset(ptr, 0xDD, slab->obj_size);

    h->magic = CHMML_MAGIC_FREE;
    h->file  = NULL;
    h->line  = 0;
    h->func  = NULL;
    h->next  = slab->free_list;
    h->prev  = NULL;
    slab->free_list = h;
    slab->free_count++;

    InterlockedAdd64(&g_heap.bytes_used, -(LONGLONG)slab->obj_size);
    InterlockedIncrement64(&g_heap.total_frees);
}

/* ======================================================
   Large object alloc
   ====================================================== */
static void *large_alloc(size_t req_size,
                           const char *file, int line, const char *func) {
    size_t aligned = align_up(req_size, CHMML_ALIGN);
    size_t total   = HDR_SZ + CHMML_REDZONE + aligned;

    uint8_t *mem = (uint8_t *)malloc(total);
    if (!mem) return NULL;

    CHMML_Header *h = (CHMML_Header *)mem;
    h->canary_pre  = CHMML_CANARY_PRE;
    h->magic       = CHMML_MAGIC;
    h->user_size   = (uint32_t)req_size;
    h->slab_class  = 0xFFFF;
    h->alloc_id    = g_heap.next_id++;
    h->file        = file;
    h->line        = line;
    h->func        = func;
    h->canary_post = CHMML_CANARY_POST;

    /* لیست linked */
    h->next = g_heap.large_list;
    h->prev = NULL;
    if (g_heap.large_list) g_heap.large_list->prev = h;
    g_heap.large_list = h;

    uint8_t *user_ptr = mem + HDR_SZ;
    if (g_heap.enable_canary) {
        uint64_t *rz = (uint64_t *)user_ptr;
        *rz = CHMML_CANARY_PRE;
    }

    void *ret = user_ptr + CHMML_REDZONE;

    InterlockedAdd64(&g_heap.bytes_used, (LONGLONG)aligned);
    InterlockedIncrement64(&g_heap.total_allocs);
    if (g_heap.bytes_used > g_heap.bytes_peak)
        InterlockedExchange64(&g_heap.bytes_peak, g_heap.bytes_used);

    if (file && func)
        track_add(ret, req_size, file, line, func);

    return ret;
}

/* ======================================================
   Large object free
   ====================================================== */
static void large_free(void *ptr) {
    CHMML_Header *h = (CHMML_Header *)((uint8_t *)ptr
                       - CHMML_REDZONE - HDR_SZ);

    if (h->magic == CHMML_MAGIC_FREE) {
        OutputDebugStringA("[CHMML] DOUBLE FREE (large) detected!\n");
        return;
    }
    if (h->magic != CHMML_MAGIC || h->slab_class != 0xFFFF) {
        OutputDebugStringA("[CHMML] INVALID PTR (large) in free!\n");
        return;
    }

    if (g_heap.enable_canary) {
        uint64_t *rz = (uint64_t *)((uint8_t *)h + HDR_SZ);
        if (*rz != CHMML_CANARY_PRE) {
            InterlockedIncrement64(&g_heap.overflows);
            OutputDebugStringA("[CHMML] HEAP OVERFLOW (large) detected!\n");
        }
    }

    track_remove(ptr);

    /* unlink از large list */
    if (h->prev) h->prev->next = h->next;
    else         g_heap.large_list = h->next;
    if (h->next) h->next->prev = h->prev;

    size_t aligned = align_up(h->user_size, CHMML_ALIGN);
    InterlockedAdd64(&g_heap.bytes_used, -(LONGLONG)aligned);
    InterlockedIncrement64(&g_heap.total_frees);

    h->magic = CHMML_MAGIC_FREE;
    free(h);
}

/* ======================================================
   Init heap
   ====================================================== */
static void chmml_heap_init(bool enable_canary, bool enable_leak_report) {
    if (g_heap_ready) return;

    memset(&g_heap, 0, sizeof(g_heap));
    InitializeCriticalSectionAndSpinCount(&g_heap.lock, 4000);

    g_heap.enable_canary       = enable_canary;
    g_heap.enable_leak_report  = enable_leak_report;

    for (int i = 0; i < CHMML_SLAB_CLASSES; i++) {
        g_heap.slabs[i].obj_size   = slab_sizes[i];
        g_heap.slabs[i].block_size = align_up(
            HDR_SZ + CHMML_REDZONE + slab_sizes[i], CHMML_ALIGN);
    }

    g_heap.track_cap   = CHMML_MAX_TRACKED;
    g_heap.track_table = (CHMML_AllocInfo *)calloc(
        CHMML_MAX_TRACKED, sizeof(CHMML_AllocInfo));

    g_heap_ready = true;
    LOGI(LOG_MOD_CHMML, "heap initialized (canary=%s, leak_report=%s)",
         enable_canary      ? "ON" : "OFF",
         enable_leak_report ? "ON" : "OFF");
}

/* ======================================================
   Shutdown heap
   ====================================================== */
static void chmml_heap_shutdown(void) {
    if (!g_heap_ready) return;

    if (g_heap.enable_leak_report)
        chmml_report_leaks();

    /* آزاد کردن large objects */
    CHMML_Header *h = g_heap.large_list;
    while (h) {
        CHMML_Header *next = h->next;
        free(h);
        h = next;
    }

    /* آزاد کردن slab pages */
    for (int i = 0; i < CHMML_SLAB_CLASSES; i++) {
        CHMML_SlabPage *page = g_heap.slabs[i].pages;
        while (page) {
            CHMML_SlabPage *next = page->next;
            free(page);
            page = next;
        }
    }

    free(g_heap.track_table);
    DeleteCriticalSection(&g_heap.lock);
    g_heap_ready = false;
}

/* ======================================================
   Public malloc/free API
   ====================================================== */
void *chmml_malloc(size_t size,
                    const char *file, int line, const char *func) {
    if (size == 0 || !g_heap_ready) return NULL;

    EnterCriticalSection(&g_heap.lock);
    int ci = find_slab_class(size);
    void *ptr = (ci >= 0)
                ? slab_alloc(ci, size, file, line, func)
                : large_alloc(size, file, line, func);
    LeaveCriticalSection(&g_heap.lock);
    return ptr;
}

void *chmml_calloc(size_t count, size_t size,
                    const char *file, int line, const char *func) {
    size_t total = count * size;
    void  *ptr   = chmml_malloc(total, file, line, func);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *chmml_realloc(void *ptr, size_t new_size,
                     const char *file, int line, const char *func) {
    if (!ptr) return chmml_malloc(new_size, file, line, func);
    if (new_size == 0) { chmml_free(ptr, file, line, func); return NULL; }

    /* قدیمی رو بخون */
    CHMML_Header *h = (CHMML_Header *)((uint8_t *)ptr
                       - CHMML_REDZONE - HDR_SZ);
    if (h->magic != CHMML_MAGIC) return NULL;
    size_t old_size = h->user_size;

    void *new_ptr = chmml_malloc(new_size, file, line, func);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, (old_size < new_size) ? old_size : new_size);
    chmml_free(ptr, file, line, func);
    return new_ptr;
}

void chmml_free(void *ptr,
                 const char *file, int line, const char *func) {
    (void)file; (void)line; (void)func;
    if (!ptr || !g_heap_ready) return;

    EnterCriticalSection(&g_heap.lock);
    CHMML_Header *h = (CHMML_Header *)((uint8_t *)ptr
                       - CHMML_REDZONE - HDR_SZ);
    if (h->slab_class == 0xFFFF)
        large_free(ptr);
    else if (h->slab_class < CHMML_SLAB_CLASSES)
        slab_free(ptr, (int)h->slab_class);
    LeaveCriticalSection(&g_heap.lock);
}

/* ======================================================
   آمار و گزارش
   ====================================================== */
void chmml_print_stats(void) {
    if (!g_heap_ready) return;
    LOGI(LOG_MOD_CHMML,
         "used=%lld peak=%lld "
         "allocs=%lld frees=%lld "
         "slab_hits=%lld misses=%lld "
         "overflows=%lld",
         (long long)g_heap.bytes_used, (long long)g_heap.bytes_peak,
         (long long)g_heap.total_allocs, (long long)g_heap.total_frees,
         (long long)g_heap.slab_hits, (long long)g_heap.slab_misses,
         (long long)g_heap.overflows);
}

void chmml_report_leaks(void) {
    if (!g_heap_ready || !g_heap.track_table) return;

    int    leak_count = 0;
    size_t leak_bytes = 0;

    for (int i = 0; i < g_heap.track_cap; i++) {
        CHMML_AllocInfo *info = &g_heap.track_table[i];
        if (!info->ptr) continue;
        leak_count++;
        leak_bytes += info->size;
    }

    if (leak_count == 0) {
        LOGI(LOG_MOD_CHMML, "No leaks detected.");
        return;
    }

    LOGW(LOG_MOD_CHMML, "*** %d LEAK(S) DETECTED: %zu bytes ***",
         leak_count, leak_bytes);

    for (int i = 0; i < g_heap.track_cap; i++) {
        CHMML_AllocInfo *info = &g_heap.track_table[i];
        if (!info->ptr) continue;
        LOGW(LOG_MOD_CHMML, "  #%u  %zu bytes  at  %s:%d  [%s]",
             info->alloc_id, info->size,
             info->file ? info->file : "?",
             info->line,
             info->func ? info->func : "?");
    }
}

size_t chmml_bytes_in_use(void) {
    return g_heap_ready ? (size_t)g_heap.bytes_used : 0;
}

/* ======================================================
   Plugin hooks
   ====================================================== */
IDM_PluginHooks *g_chmml_plugin_instance = NULL;

static int hook_init(void) {
    /* heap قبلاً در create_plugin راه‌اندازی شده */
    LOGI(LOG_MOD_CHMML, "plugin active");
    return 0;
}

static void hook_destroy(void) {
    chmml_print_stats();
    chmml_heap_shutdown();
    LOGI(LOG_MOD_CHMML, "plugin destroyed");
}

/* گزارش آمار هر بار که دانلود کامل می‌شه */
static void hook_dl_done(IDM_Download *dl) {
    (void)dl;
    chmml_print_stats();
}

/* ======================================================
   Public plugin API
   ====================================================== */
IDM_PluginHooks *chmml_plugin_create(bool enable_leak_report,
                                      bool enable_canary) {
    chmml_heap_init(enable_canary, enable_leak_report);

    IDM_PluginHooks *plugin = (IDM_PluginHooks *)calloc(1, sizeof(*plugin));
    if (!plugin) return NULL;

    plugin->name             = "chmml";
    plugin->version          = "2.2";
    plugin->api_version      = IDM_PLUGIN_API_VERSION;
    plugin->priv             = NULL;
    plugin->on_init          = hook_init;
    plugin->on_destroy       = hook_destroy;
    plugin->on_download_done = hook_dl_done;

    g_chmml_plugin_instance = plugin;
    return plugin;
}

void chmml_plugin_destroy(IDM_PluginHooks *plugin) {
    if (!plugin) return;
    if (g_chmml_plugin_instance == plugin)
        g_chmml_plugin_instance = NULL;
    free(plugin);
}
