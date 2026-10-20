# Lass v5
دانلود منیجر سریع برای ویندوز

---

## ساختار پروژه

```
Lass/
│
├── src/               ← کد اصلی (هرگز تغییر نمی‌کند)
│   ├── types.h        - تعریف‌های پایه
│   ├── http.h/c       - HTTP با WinINet (بدون libcurl)
│   ├── storage.h/c    - فایل‌نویسی با OVERLAPPED IO
│   ├── segment.h/c    - هر بخش دانلود، thread جداگانه
│   ├── resume.h/c     - ذخیره/بازیابی حالت
│   ├── engine.h/c     - موتور مرکزی و صف دانلود
│   └── main.c         - GUI با WinAPI
│
├── plugins/           ← پلاگین‌های کمکی (قابل فعال/غیرفعال)
│   ├── plugin_api.h/c          - رابط پلاگین (hook system)
│   ├── plugins_setup.h/c       - سیم‌کشی همه پلاگین‌ها
│   ├── engine_bridge.h/c       - اتصال پلاگین به engine
│   ├── security_fixes.h        - رفع باگ‌های امنیتی
│   ├── plugin_smart_resume.h/c - ذخیره هوشمند state
│   ├── plugin_dynamic_seg.h/c  - split/merge خودکار
│   ├── plugin_cookie_jar.h/c   - مدیریت cookie
│   └── plugin_chmml.h/c        - memory leak detector
│
├── tests/
│   └── test_all.c     ← تست یکپارچه همه ماژول‌ها
│
├── build.bat          ← build با MinGW64
└── CMakeLists.txt     ← build با CMake

```

---

## ویژگی‌ها

| Speed Graph | نمودار سرعت GDI با ring buffer 60 ثانیه |

| ویژگی | توضیح |
|-------|--------|
| Multi-segment | تا ۱۶ بخش موازی |
| Resume | ادامه دانلود قطع‌شده |
| Smart Resume | fingerprint + CRC32 + XOR |
| Dynamic Split | تقسیم خودکار بخش‌های سریع |
| Stall Detection | راه‌اندازی مجدد بخش‌های کند |
| Cookie Jar | برای سایت‌های نیاز به login |
| Speed Limit | محدودیت سرعت قابل تنظیم |
| CHMML | memory leak detector (debug) |
| بدون dependency | فقط DLL‌های پیش‌فرض ویندوز |

---

## Build

### پیش‌نیاز
MinGW-w64 (MSYS2 یا winlibs)

### روش سریع
```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

سه خروجی تولید می‌شه:
- `build\Lass.exe` - نسخه اصلی
- `build\Lass.exe` (پیکربندی Debug) - CHMML leak detector فعال است
- `build\test_all.exe` - تست یکپارچه

### روش CMake
```bat
cmake -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build
```

### CMake با Debug
```bat
cmake -B build -G "MinGW Makefiles" -DIDM_DEBUG=ON
cmake --build build
```

---

## پلاگین‌ها

### Smart Resume
فایل state با پسوند `.dat` در `%APPDATA%\Lass\states\` ذخیره می‌شه.
- fingerprint با FNV-1a hash
- CRC32 روی هر segment
- رمزگذاری XOR
- تشخیص تغییر فایل روی سرور

### Dynamic Segment
هر ۲ ثانیه segment‌ها بررسی می‌شن:
- سرعت بالای ۵۱۲ KB/s → split
- بی‌حرکتی → restart

### Cookie Jar
فرمت Netscape (سازگار با Firefox/Chrome export):
```
cookie_jar_set(plug, "site.com", "session", "token123", 3600);
cookie_jar_import(plug, "C:\\cookies.txt");
```

### CHMML (Debug)
فقط در debug build فعاله (`-DIDM_DEBUG`).
گزارش leak در stderr هنگام خروج.

---

## امنیت
- URL validation (فقط http/https، بدون control char)
- Path traversal protection (`../` block)
- Integer overflow check در calloc
- Double-free detection
- Buffer overread protection
