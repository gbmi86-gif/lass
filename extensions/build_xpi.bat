@echo off
REM ساخت فایل .xpi از پوشه‌ی lass-firefox برای نصب موقت در Firefox
REM (Firefox عادی فقط از طریق about:debugging اجازه‌ی نصب extension
REM  امضانشده رو میده — حتی .xpi هم باید یا امضا بشه یا موقت باشه)

cd /d "%~dp0lass-firefox"
if exist "..\lass-firefox.xpi" del "..\lass-firefox.xpi"

REM نیاز به PowerShell (پیش‌فرض در ویندوز 10/11 موجوده)
powershell -Command "Compress-Archive -Path '.\*' -DestinationPath '..\lass-firefox.xpi' -Force"

echo.
echo ساخته شد: extensions\lass-firefox.xpi
echo.
echo نکته مهم: Firefox عادی extension امضانشده رو دائمی نصب نمی‌کنه.
echo برای تست: Firefox را باز کن، به about:debugging#/runtime/this-firefox برو،
echo روی "Load Temporary Add-on" کلیک کن و مستقیم فایل manifest.json
echo (نه این xpi) را از پوشه‌ی lass-firefox انتخاب کن.
echo این روش موقتی است و با بستن Firefox پاک می‌شود.
pause
