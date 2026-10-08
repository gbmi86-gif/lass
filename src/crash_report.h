/* crash_report.h — جزئیات یه exception رو در لاگ ثبت می‌کنه.

   قبلاً crash handler فقط کد exception و یه آدرس خام می‌نوشت. با
   ASLR آدرس خام بدون module base بی‌معناست، و هیچ سرنخی نبود که
   خطا داخل کد خود Lass بوده یا داخل یه DLL ویندوز (مثلاً یه API
   که اشاره‌گر بد گرفته) یا کی اونو صدا زده.

   حالا برای محل خطا و هر frame از call stack، «نام ماژول+offset»
   ثبت می‌شه (مثلاً  Lass.exe+0x1C04B  یا  SHLWAPI.dll+0x2A10).
   برای تبدیل به نام تابع/خط سورس:
       x86_64-w64-mingw32-addr2line -e Lass.exe -f -C -p 0x<ImageBase+offset>
   (ImageBase پیش‌فرض mingw 64-bit برابر 0x140000000 هست؛ با
   objdump -p Lass.exe | findstr ImageBase چک کن.) */
#ifndef LASS_CRASH_REPORT_H
#define LASS_CRASH_REPORT_H

#include <windows.h>

/* محل خطا + call stack رو با LOGE می‌نویسه. تعداد frame های
   walk‌شده رو برمی‌گردونه (روی x64؛ روی معماری‌های دیگه 0).
   هرگز crash نمی‌کنه — خوندن حافظه با ReadProcessMemory انجام
   می‌شه، نه دسترسی مستقیم. */
int crash_report_log(EXCEPTION_POINTERS *ep);

#endif
