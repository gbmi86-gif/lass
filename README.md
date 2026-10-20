# Lass Download Manager v5

A fast, plugin-based download manager for Windows — built from scratch with Win32 and WinINet, with zero external dependencies.

---

## ✨ Features
- **Multi-segment:** Up to 16 parallel connections.
- **Resume:** Resume interrupted downloads.
- **Segment Park:** Pauses a segment after 10 retries without progress.
- **Retry-After:** Respects server requests.
- **FTP:** Independent WinSock with REST support.
- **Streaming:** HLS / DASH / RTMP (native).
- **Video Sniffer:** YouTube, Aparat, Instagram, Twitter, Vimeo.
- **Browser Extension:** Chrome / Firefox / Edge.
- **Cookie Jar:** RFC 6265 compliant.
- **Proxy:** HTTP / SOCKS4 / SOCKS5.
- **HTTP Auth:** Basic / Digest.
- **Speed Scheduler:** Hourly speed limits.
- **Speed Graph:** Visualized with GDI.
- **Categories:** Video, Audio, Document, Software, Image.
- **Extras:** Clipboard Monitor, Download Basket, Notifications (toast + tray), Persian + English UI (i18n).

## 🛡️ Security
- **AES-GCM encryption:** For `.tels` container.
- **DPAPI:** For keys (user + machine bound).
- **URL credentials protection:** Split / redact / encrypted store.
- **Filename sanitization:** Path traversal / illegal chars.
- **Content-Length validation:** DoS protection.
- **Command injection protection:** Prevents malicious execution.
- **Integer overflow checks:** Safe memory handling.
- **Test isolation:** `lass_paths` ensures the real profile remains untouched.

---

## 🏗️ Architecture

```text
Lass/
├── src/                 ← Core code (19 files)
│   ├── types.h
│   ├── http.c/h         ← HTTP with WinINet
│   ├── transport.c/h    ← Abstraction for HTTP/FTP
│   ├── storage.c/h      ← File writing with OVERLAPPED IO
│   ├── tels_container.c/h ← Transactional container
│   ├── lass_store.c/h   ← Key-Value Store
│   ├── segment.c/h      ← Each segment, separate thread
│   ├── resume.c/h       ← Save/restore
│   ├── engine.c/h       ← Central engine
│   ├── queue_persist.c/h
│   ├── lass_paths.c/h   ← Test isolation
│   ├── crash_report.c/h
│   ├── shutdown_brake.c/h
│   ├── secure_store.c/h
│   ├── checksum.c/h
│   ├── legacy_data.c/h
│   ├── url_secure.c/h
│   ├── lass_log.c/h
│   ├── settings.c/h
│   └── i18n.c/h
├── plugins/             ← 20 plugins
│   ├── plugin_api.c
│   ├── plugins_setup.c
│   ├── plugin_smart_resume.c
│   ├── plugin_dynamic_seg.c
│   ├── plugin_cookie_jar.c
│   ├── plugin_chmml.c
│   ├── plugin_speed_graph.c
│   ├── plugin_clipboard.c
│   ├── plugin_basket.c
│   ├── plugin_ftp.c
│   ├── plugin_hls.c
│   ├── plugin_dash.c
│   ├── plugin_proxy.c
│   ├── plugin_http_auth.c
│   ├── plugin_browser_ext.c
│   ├── plugin_notify.c
│   ├── plugin_categories.c
│   ├── plugin_rtmp.c
│   ├── plugin_video_sniffer.c
│   └── plugin_speed_scheduler.c
├── core/
│   ├── core_ldm.c/h     ← Central API
│   └── guo.c/h          ← UI (GUI)
├── tests/               ← 762/767 tests passing
│   ├── test_all.c
│   ├── test_httpd.c
│   └── test_ftpd.c
├── build.bat
├── CMakeLists.txt
├── LICENSE              ← GPL-3.0
└── README.md
🔨 Building
Requirements: MinGW-w64 (MSYS2 or winlibs)

Method 1: CMake

bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
🇮🇷 معرفی (Persian)
دانلود منیجر سریع برای ویندوز — ساخته‌شده با Win32 و WinINet، بدون هیچ dependency خارجی.

✨ ویژگی‌ها
Multi-segment: تا ۱۶ اتصال موازی.

Resume: ادامه دانلود قطع‌شده.

Segment Park: توقف segment بعد از ۱۰ تلاش بدون پیشرفت.

Retry-After: احترام به درخواست سرور.

FTP: WinSock مستقل، با REST support.

Streaming: HLS / DASH / RTMP (native).

Video Sniffer: یوتیوب، آپارات، اینستاگرام، توییتر، ویمیو.

Browser Extension: کروم، فایرفاکس، اج.

Cookie Jar: مطابق با RFC 6265.

Proxy: HTTP / SOCKS4 / SOCKS5.

HTTP Auth: Basic / Digest.

Speed Scheduler: محدودیت سرعت ساعتی.

Speed Graph: نمودار سرعت با GDI.

Categories: ویدیو، صدا، سند، نرم‌افزار، تصویر.

سایر امکانات: Clipboard Monitor، Download Basket، اعلان‌ها (toast + tray)، رابط کاربری فارسی و انگلیسی (i18n).

🛡️ امنیت
AES-GCM encryption: برای .tels container.

DPAPI: برای کلید (محدود به user + machine).

URL credentials protection: جداسازی، پاک‌سازی و ذخیره‌سازی رمزنگاری‌شده.

Filename sanitization: جلوگیری از path traversal و کاراکترهای غیرمجاز.

Content-Length validation: محافظت در برابر DoS.

Command injection protection: جلوگیری از اجرای دستورات مخرب.

Integer overflow checks: مدیریت ایمن حافظه.

Test isolation: lass_paths تضمین می‌کند پروفایل واقعی دست‌نخورده بماند.

🔨 بیلد کردن
نیازمندی‌ها: MinGW-w64 (MSYS2 یا winlibs)

bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
