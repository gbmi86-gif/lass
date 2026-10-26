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

## Why Lass?

- **Open by design:** Every line is GPL-3.0. You can read it, change it, 
  learn from it, or build your own project on top of it.
- **Modular:** The download engine, key-value store, container format, 
  and browser pipe are separate modules. Replace one without touching the rest.
- **Transparent:** No hidden telemetry, no closed binaries. If there's a bug, 
  it's in the open — and you can fix it.
- **Plugin-friendly:** Native messaging for Chrome, Firefox, and Edge.
- **Lightweight:** No Java runtime, no Electron. Just C and Win32.
- **It negotiates, it doesn't dictate.** Most download managers 
  ignore the server. Lass reads Retry-After, parks segments that 
  make no progress, reduces concurrency on troubled hosts, and 
  sends If-Range for smart resume. It respects the server — and 
  the server respects it back.

## Design philosophy: negotiate, don't dictate

Most download managers treat the server as a dumb pipe: they open 
as many connections as possible, retry aggressively, and ignore 
what the server is telling them. This works — until the server 
rate-limits you, parks your IP, or starts returning `429`.

Lass takes a different approach. It **listens** to the server 
and adapts.

### What "negotiation" means in practice

- **Reads `Retry-After`** on `429`/`503` and waits the requested 
  time (capped at 5 minutes) instead of hammering the server.
- **Parks stalled segments** after 10 consecutive attempts 
  without progress, and records *why* each segment failed.
- **Reduces concurrency on troubled hosts** (`engine_report_host_trouble`): 
  a host that keeps failing gets fewer segments next time, 
  automatically.
- **Sends `If-Range` with the stored `ETag`** so resume works 
  correctly even if the file changed on the server.
- **Distinguishes permanent from transient errors**: `404`/`410`/`400`/`401` 
  stop immediately; `429`/`500`/`503` are retried with backoff.

The result is a download that's **slower to start but faster to 
finish** — and a server that stays healthy for the next user.

### Why we changed

Early builds (v4) were speed-first: aggressive segmentation, 
constant retries, maximum throughput. They were fast — sometimes 
faster than IDM — but they hammered servers and got rate-limited. 
On a rate-limited server, v4 could hit ~1 MB/s while opening 8+ 
connections; the server throttled each one to ~125 KB/s.

Starting with v5.0.49, Lass switched to a **negotiation-first** 
model. On the same rate-limited server, v5 reaches ~25 MB/s by 
noticing which connections are throttled, parking them, and 
keeping the fast one alive. Same server, same file — **25× faster**, 
because it listens instead of dictating.

### Responsible use

Lass is designed to negotiate, not to abuse. Please:

- Respect server rate limits and terms of service.
- Don't use Lass to hammer servers that are clearly throttling you.
- Use the built-in speed limiter if you need a hard cap.

The goal is a download manager that's **fast because it's smart**, 
not fast because it's rude.
## FAQ

### Is Lass a replacement for IDM?

No. IDM is a mature, closed-source product with years of 
development and millions of users. Lass is a smaller, 
open-source project with a different philosophy. If you want a 
polished, set-and-forget download manager, IDM or FDM is a 
better fit. If you want a download manager you can read, 
modify, and extend, Lass is for you.

### Why is v5 slower than v4?

v4 was speed-first: aggressive segmentation, constant retries, 
maximum throughput. It was fast — sometimes faster than IDM — but 
it hammered servers and got rate-limited. On a rate-limited 
server, v4 could hit ~1 MB/s while opening 8+ connections; the 
server throttled each one to ~125 KB/s.

v5 switched to a **negotiation-first** model (starting with 
v5.0.49). It reads `Retry-After`, parks stalled segments, and 
reduces concurrency on troubled hosts. The result is a download 
that's slower to *start* but faster to *finish* — and a server 
that stays healthy for the next user. On the same rate-limited 
server, v5 reaches ~25 MB/s because it listens instead of 
dictating.

### Does Lass hammer servers?

No — and that's the point. Lass is designed to negotiate, not 
to abuse. It reads `Retry-After`, parks segments that make no 
progress, and reduces concurrency on hosts that keep failing. 
If you need a hard cap, use the built-in speed limiter.

### Why Windows-only?

Lass is built on Win32 APIs (`MoveFileEx`, `CRITICAL_SECTION`, 
DPAPI, native messaging pipes). Porting it to Linux or macOS 
would mean rewriting the storage layer, the encryption layer, 
and the browser integration. That's a significant effort, and 
it's not on the roadmap right now. If you want a cross-platform 
open-source download manager, look at aria2 or JDownloader.

### Why GPL-3.0?

Because transparency matters. GPL-3.0 ensures that anyone who 
distributes a modified version of Lass must also share their 
source. This keeps the project open — not just in name, but in 
practice. If you use Lass in your own project, you must also 
release your source under GPL-3.0.

### Can I use Lass commercially?

GPL-3.0 allows commercial use, but any distributed modification 
must also be GPL-3.0. If you need a different license (e.g., for 
a closed-source product), contact the maintainer.

### Does Lass support BitTorrent?

Not yet. BitTorrent is on the wishlist, but it's a significant 
undertaking (DHT, peer protocol, piece verification). The current 
focus is on HTTP/HTTPS/FTP and streaming protocols (HLS, DASH, 
RTMP).

### Does Lass collect telemetry?

No. Lass has no telemetry, no analytics, no phone-home. Every 
line of code is in this repository. If you don't believe me, 
read it.

### How do I report a bug?

Open an issue on GitHub with:
- The version of Lass (shown in the title bar and About dialog).
- Steps to reproduce.
- The relevant lines from `lass.log` (if any).
- Your Windows version and toolchain (if building from source).

Please don't paste passwords or full URLs with credentials. 
Lass redacts passwords in logs, but double-check before posting.

### Can I contribute?

Yes! Pull requests are welcome. Please:
- Keep changes focused (one feature or fix per PR).
- Add tests for new behavior.
- Run the test suite (`test_all.exe`) and make sure it passes.
- Follow the existing code style (C11, Win32, no external 
  dependencies).

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

📄 License
This project is licensed under the GPL-3.0 License.
