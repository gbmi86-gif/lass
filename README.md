<img width="1114" height="743" alt="Screenshot 2026-10-18 123512" src="https://github.com/user-attachments/assets/b728a3a2-dcf2-426f-948e-75d2a1c4b574" />
# Lass Download Manager v5

دانلود منیجر سریع برای ویندوز — ساخته‌شده با Win32 و WinINet، بدون هیچ dependency خارجی.

A fast, plugin-based download manager for Windows — built from scratch with Win32 and WinINet.

## ✨ Features

- **Multi-segment** (تا ۱۶ اتصال موازی)
- **Resume** (ادامه دانلود قطع‌شده)
- **Segment Park** (توقف segment بعد از ۱۰ تلاش بدون پیشرفت)
- **Retry-After** (احترام به درخواست سرور)
- **FTP** (WinSock مستقل، با REST support)
- **HLS / DASH / RTMP** (native)
- **Video Sniffer** (YouTube, Aparat, Instagram, Twitter, Vimeo)
- **Browser Extension** (Chrome / Firefox / Edge)
- **Cookie Jar** (RFC 6265)
- **Proxy** (HTTP / SOCKS4 / SOCKS5)
- **HTTP Auth** (Basic / Digest)
- **Speed Scheduler** (محدودیت سرعت ساعتی)
- **Speed Graph** (نمودار سرعت با GDI)
- **Categories** (Video, Audio, Document, Software, Image)
- **Clipboard Monitor**
- **Download Basket**
- **Notifications** (toast + tray)
- **Persian + English UI** (i18n)

## 🛡️ Security

- **AES-GCM encryption** برای `.tels` container
- **DPAPI** برای کلید (user + machine bound)
- **URL credentials protection** (split / redact / encrypted store)
- **Filename sanitization** (path traversal / illegal chars)
- **Content-Length validation** (DoS protection)
- **Command injection protection**
- **Integer overflow checks**
- **Test isolation** (`lass_paths` — پروفایل واقعی دست‌نخورده)

## 🏗️ Architecture

Lass/
├── src/               ← کد اصلی (۱۹ فایل)
│   ├── types.h
│   ├── http.c/h       ← HTTP با WinINet
│   ├── transport.c/h  ← abstraction برای HTTP/FTP
│   ├── storage.c/h    ← فایل‌نویسی با OVERLAPPED IO
│   ├── tels_container.c/h  ← transactional container
│   ├── lass_store.c/h ← Key-Value Store
│   ├── segment.c/h    ← هر بخش، thread جداگانه
│   ├── resume.c/h     ← ذخیره/بازیابی
│   ├── engine.c/h     ← موتور مرکزی
│   ├── queue_persist.c/h
│   ├── lass_paths.c/h ← test isolation
│   ├── crash_report.c/h
│   ├── shutdown_brake.c/h
│   ├── secure_store.c/h
│   ├── checksum.c/h
│   ├── legacy_data.c/h
│   ├── url_secure.c/h
│   ├── lass_log.c/h
│   ├── settings.c/h
│   └── i18n.c/h
│
├── plugins/           ← ۲۰ پلاگین
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
│
├── core/
│   ├── core_ldm.c/h   ← API مرکزی
│   └── guo.c/h        ← UI (GUI)
│
├── tests/             ← ۷۶۲/۷۶۷ تست
│   ├── test_all.c
│   ├── test_httpd.c
│   └── test_ftpd.c
│
├── build.bat
├── CMakeLists.txt
├── LICENSE            ← GPL-3.0
└── README.md

## 🔨 Building

**نیازمندی‌ها:** MinGW-w64 (MSYS2 یا winlibs)

**روش ۱: CMake**
```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
