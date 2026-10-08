/* plugin_chmml.h
   ─────────────────────────────────────────────────────────────
   پلاگین ۴: CHMML - Debug Memory Manager
   بر اساس الگوریتم اصلی کاربر، بازنویسی شده برای MinGW64:
   - Slab allocator با ۸ size class
   - Canary guard (overflow/underflow detection)
   - Leak tracking با allocation table
   - Thread-safe با CRITICAL_SECTION (نه pthread)
   - گزارش نشت در shutdown
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_CHMML_H
#define IDM_PLUGIN_CHMML_H

#include "plugin_api.h"

/* ---- ساخت پلاگین ---- */
IDM_PluginHooks *chmml_plugin_create(bool enable_leak_report,
                                      bool enable_canary);
void             chmml_plugin_destroy(IDM_PluginHooks *plugin);

/* ---- API مستقیم (برای استفاده جای malloc/free) ---- */
void  *chmml_malloc (size_t size,
                     const char *file, int line, const char *func);
void  *chmml_calloc (size_t count, size_t size,
                     const char *file, int line, const char *func);
void  *chmml_realloc(void *ptr, size_t new_size,
                     const char *file, int line, const char *func);
void   chmml_free   (void *ptr,
                     const char *file, int line, const char *func);

void   chmml_print_stats  (void);
void   chmml_report_leaks (void);
size_t chmml_bytes_in_use (void);

/* ---- ماکروهای راحت ---- */
#ifdef IDM_USE_CHMML
  #define IDM_MALLOC(sz)        chmml_malloc ((sz),   __FILE__, __LINE__, __func__)
  #define IDM_CALLOC(n, sz)     chmml_calloc ((n),(sz),__FILE__, __LINE__, __func__)
  #define IDM_REALLOC(p, sz)    chmml_realloc((p),(sz),__FILE__, __LINE__, __func__)
  #define IDM_FREE(p)           chmml_free   ((p),    __FILE__, __LINE__, __func__)
#else
  #define IDM_MALLOC(sz)        malloc(sz)
  #define IDM_CALLOC(n, sz)     calloc((n),(sz))
  #define IDM_REALLOC(p, sz)    realloc((p),(sz))
  #define IDM_FREE(p)           free(p)
#endif

#endif /* IDM_PLUGIN_CHMML_H */
