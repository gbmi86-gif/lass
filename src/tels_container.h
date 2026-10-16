/* tels_container.h — کانتینر «.tels»: یک فایل که داخلش یک سیستم فایل مجازی دارد.

   کاربر فقط یک فایل می‌بیند؛ داخلش هزاران entry (داده، پوشه، state) و کانتینرهای
   تودرتو می‌تواند باشد. ساختار:
     - دو superblock برای commit اتمیک (هر commit یکی از آن‌ها را جلو می‌برد)
     - لاگی از رکوردهای خودتوصیف: ساخت، داده در آفست، حذف، و index
   بازیابی: superblock معتبر جدیدتر، index آن، و replay رکوردهای معتبر بعدی.
   رکورد نیمه‌نوشته‌ی انتهای فایل دور ریخته می‌شود. هر رکورد CRC32 دارد.

   thread-safe است: همه‌ی عملیات با یک critical section محافظت می‌شوند. */
#ifndef LASS_TELS_CONTAINER_H
#define LASS_TELS_CONTAINER_H

#include <windows.h>
#include <stdint.h>

#define TELS_ROOT           0u
#define TELS_MAX_ENTRIES    65536u
#define TELS_MAX_DEPTH      16u          /* تعداد کانتینر تودرتو */
#define TELS_MAX_NAME       255u
#define TELS_MAX_RECORD     (16u * 1024u * 1024u)

typedef uint32_t TelsId;
typedef struct TelsContainer TelsContainer;
typedef enum { TELS_TYPE_FILE = 1, TELS_TYPE_DIR = 2 } TelsType;

/* باز یا ایجاد می‌کند. create=TRUE: اگر فایل نباشد می‌سازد. */
TelsContainer *tels_open(const char *path, BOOL create);
/* commit می‌کند و می‌بندد */
void           tels_close(TelsContainer *c);
/* بدون commit می‌بندد (برای شبیه‌سازی کرش در تست‌ها) */
void           tels_abort(TelsContainer *c);
/* state فعلی را به‌عنوان نقطه‌ی بازیابی در فایل ثبت می‌کند */
BOOL           tels_commit(TelsContainer *c);

/* parent=TELS_ROOT برای ریشه. نام‌ها بدون '/', '\\', ':' و کنترل؛ مقایسه بدون حساسیت به حروف */
BOOL     tels_create(TelsContainer *c, TelsId parent, const char *name, TelsType type, TelsId *out);
BOOL     tels_lookup(TelsContainer *c, TelsId parent, const char *name, TelsId *out);
/* داده را در آفست مشخص می‌نویسد؛ فاصله‌ها هنگام خواندن صفر هستند */
BOOL     tels_write(TelsContainer *c, TelsId id, uint64_t offset, const void *buf, uint32_t len);
/* >0 بایت خوانده‌شده، 0 در پایان، -1 خطا */
int32_t  tels_read(TelsContainer *c, TelsId id, uint64_t offset, void *buf, uint32_t len);
BOOL     tels_size(TelsContainer *c, TelsId id, uint64_t *size_out);
/* حذف entry همراه با تمام زیرمجموعه‌ها */
BOOL     tels_delete(TelsContainer *c, TelsId id);
uint32_t tels_list(TelsContainer *c, TelsId parent, TelsId *out, uint32_t cap);
BOOL     tels_info(TelsContainer *c, TelsId id, TelsType *type, TelsId *parent,
                   char *name, size_t name_sz);
/* مسیر "a/b/c" را با جداکننده‌ی '/' یا '\\' می‌پیماید؛ create_dirs=TRUE پوشه‌های گم‌شده را می‌سازد */
BOOL     tels_resolve(TelsContainer *c, const char *path, BOOL create_dirs, TelsId *out);

#endif
