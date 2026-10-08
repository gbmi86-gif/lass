# ساخت نصاب Lass (installer)

## چیزی که این حل می‌کنه
تا الان `Lass.exe` مستقیم اجرا می‌شد — یعنی برای اجرا روی یه سیستم
دیگه، هم باید DLLهای runtime خودِ MinGW همراهش می‌بود، هم هیچ
نصب/حذف تمیزی (میانبر، Add/Remove Programs، و غیره) نداشت.

دو تا تغییر این رو حل کرد:
1. **Link استاتیک** (`-static` در CMakeLists.txt/build.bat) — از این به بعد
   `Lass.exe` هیچ وابستگی به DLL بیرونی نداره؛ خودش به‌تنهایی روی هر
   ویندوزی اجرا می‌شه.
2. **نصاب Inno Setup** (همین پوشه) — یه فایل واحد `LassSetup-X.X.X.exe`
   می‌سازه که Lass رو (بدون نیاز به دسترسی Admin/UAC، چون همه‌چیز
   per-user هست، هم‌راستا با بقیه‌ی برنامه) نصب می‌کنه، میانبر
   Start Menu/Desktop می‌سازه، و یه uninstaller تمیز داره.

## مراحل ساخت
1. [Inno Setup Compiler](https://jrsoftware.org/isinfo.php) رو نصب کن (رایگان، فقط یه‌بار).
2. برنامه رو عادی build کن تا `build\Lass.exe` وجود داشته باشه:
   ```
   cmake -B build
   cmake --build build --config Release
   ```
3. `installer\lass_setup.iss` رو با Inno Setup Compiler باز کن و
   Build بزن (یا از خط فرمان: `iscc installer\lass_setup.iss`).
4. خروجی: `installer\output\LassSetup-<version>.exe` — همین فایل رو
   می‌تونی به هر سیستم ویندوزی دیگه بدی، بدون نیاز به MinGW یا هیچ
   ابزار دیگه‌ای روی اون سیستم.

## نکات مهم
- شماره‌نسخه‌ی نصاب **خودکار** از خودِ `Lass.exe` (که در `resource.rc`
  با `LASS_VERSION_STRING` ست شده) خونده می‌شه — نیازی نیست
  `lass_setup.iss` رو دستی هر بار آپدیت کنی.
- نصب در `%LocalAppData%\Programs\Lass` انجام می‌شه (نه Program Files)
  — یعنی بدون هیچ پرامپت UAC.
- موقع uninstall، `Lass.exe --unregister-native-host` قبل از حذف
  خودِ exe اجرا می‌شه تا ورودی‌های ثبت‌شده‌ی مرورگر (Chrome/Firefox/
  Edge) پاک بشن — وگرنه بعد از حذف برنامه، registry به یه exe از
  بین‌رفته اشاره می‌کرد.
- تنظیمات/لاگ (`%APPDATA%\Lass`) و کلید رجیستری
  `HKCU\Software\Lass\Settings` عمداً موقع uninstall پاک نمی‌شن —
  اگه دوباره نصب کنی، تنظیماتت سرجاشه. اگه پاک‌سازی کامل هم
  می‌خوای، بگو تا یه گزینه‌ی جدا اضافه کنم.
- `AppId` داخل `.iss` یه GUID ثابته — **هیچ‌وقت** بین نسخه‌ها
  عوضش نکن، وگرنه ویندوز هر نصب رو یه برنامه‌ی جداگانه حساب
  می‌کنه به‌جای آپدیت نسخه‌ی قبلی.
