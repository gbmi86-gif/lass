# نصب Extension های Lass Download Manager

## Chrome / Edge / Whale (مرورگرهای مبتنی بر Chromium)

۱. `chrome://extensions` را باز کن (یا `edge://extensions`، `whale://extensions`)
۲. **Developer mode** را بالای صفحه فعال کن
۳. اگر قبلاً یک نسخه‌ی قدیمی‌تر از Lass Download Manager نصب کرده بودی،
   حتماً روی **Remove** کلیک کن — فقط خاموش کردن کافی نیست، چون Chrome
   ممکن است ID قدیمی را cache نگه دارد.
۴. روی **Load unpacked** کلیک کن و پوشه‌ی `extensions/lass-chrome`
   (از zip تازه استخراج‌شده، نه یک پوشه‌ی قدیمی) را انتخاب کن.
۵. باید ID زیر را ببینی — اگر متفاوت بود، یعنی هنوز نسخه‌ی قدیمی
   cache شده است، دوباره Remove و Load unpacked کن:

   ```
   kapkhgalghmdnoojfhiameibbogfjhkj
   ```

۶. در Lass، از منوی Settings → Preferences → Browser Extension →
   **Register All** بزن.
۷. برنامه‌ی Lass را باز نگه دار و روی آیکون extension در مرورگر کلیک
   کن — باید "Connected to Lass v5.0" را ببینی.

## Firefox

Firefox عادی (غیر از Developer Edition یا Nightly) به دلایل امنیتی
هیچ‌وقت اجازه‌ی نصب دائمی extension امضانشده را نمی‌دهد — این محدودیت
خود Mozilla است، نه چیزی که در کد این پروژه قابل دور زدن باشد.

### روش تست موقت (برای هر Firefox عادی کار می‌کند):

۱. `about:debugging#/runtime/this-firefox` را باز کن
۲. روی **"Load Temporary Add-on…"** کلیک کن
۳. مستقیماً فایل `extensions/lass-firefox/manifest.json` را انتخاب کن
   (نه zip، نه پوشه — دقیقاً فایل manifest.json)

   نکته: پنجره‌ی انتخاب فایل در بعضی نسخه‌های Firefox پیش‌فرضش روی
   فیلتر "ZIP/XPI Files" هست، پس manifest.json توی لیست دیده نمی‌شه
   و انگار "دنبال zip می‌گرده". این یعنی نیاز به zip کردن نیست —
   کافیه از دراپ‌دان پایین پنجره فیلتر رو به "All Files" عوض کنی،
   یا مسیر کامل manifest.json رو مستقیم توی فیلد اسم فایل تایپ/پیست
   کنی. اگه پوشه رو zip کنی، معمولاً manifest.json یه پوشه پایین‌تر
   از ریشه‌ی zip قرار می‌گیره و Firefox با خطای "این فایل خراب است"
   ردش می‌کنه — چون این روش اصلاً به zip نیازی نداره.
۴. کار می‌کند، ولی با بستن Firefox پاک می‌شود و باید دوباره این
   مراحل را تکرار کنی.

### روش دائمی (نیاز به Firefox Developer Edition یا Nightly):

۱. Firefox Developer Edition یا Nightly را نصب کن
   (https://www.mozilla.org/firefox/developer/)
۲. در `about:config`، مقدار `xpinstall.signatures.required` را
   به `false` تغییر بده
۳. فایل `.xpi` را بساز (اجرای `extensions/build_xpi.bat`) و آن را
   در Firefox drag-and-drop کن، یا از `about:addons` → گزینه‌ی
   نصب از فایل استفاده کن.

## عیب‌یابی

**"Lass is not running" حتی وقتی برنامه باز است:**
- مطمئن شو در Settings → Browser Extension روی Register All زده‌ای
- مطمئن شو Lass.exe در حال اجراست (نه فقط minimize به tray — که
  آن هم کافی است چون process زنده می‌ماند)
- بررسی کن که ID نمایش‌داده‌شده در Chrome دقیقاً همان
  `kapkhgalghmdnoojfhiameibbogfjhkj` باشد

**"This add-on could not be installed because it has not been verified" (Firefox):**
- این خطای طبیعی Firefox عادی است، نه باگ — از روش "Load Temporary
  Add-on" در بالا استفاده کن
