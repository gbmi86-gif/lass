@echo off
REM build.bat – Lass Download Manager v5.0 (MinGW64)

echo ============================================
echo  Lass Download Manager v5.0
echo  Architecture: GUO + Core + 17 Plugins
echo ============================================

where gcc >nul 2>&1
if errorlevel 1 ( echo ERROR: gcc not found. & pause & exit /b 1 )
where windres >nul 2>&1
if errorlevel 1 ( echo WARNING: windres not found - building without icon resource. )
if not exist build mkdir build

set I=-I src -I plugins -I core
set FLAGS=-std=c11 -Wall -Wextra -DWIN32 -D_WIN32 -DWIN32_LEAN_AND_MEAN %I%
set LIBS=-lwininet -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lgdi32 -lole32 -luuid -lshlwapi

set SRC=src/engine.c src/segment.c src/http.c src/storage.c src/resume.c src/settings.c src/i18n.c

set PLUGINS=^
 plugins/plugin_api.c plugins/plugins_setup.c^
 plugins/plugin_smart_resume.c plugins/plugin_dynamic_seg.c^
 plugins/plugin_cookie_jar.c plugins/plugin_chmml.c^
 plugins/plugin_speed_graph.c^
 plugins/plugin_clipboard.c^
 plugins/plugin_basket.c^
 plugins/plugin_ftp.c^
 plugins/plugin_hls.c^
 plugins/plugin_proxy.c^
 plugins/plugin_http_auth.c^
 plugins/plugin_browser_ext.c^
 plugins/plugin_dash.c^
 plugins/plugin_notify.c^
 plugins/plugin_categories.c^
 plugins/plugin_rtmp.c^
 plugins/plugin_video_sniffer.c

set CORE=core/core_ldm.c core/guo.c

echo [1/5] Compiling icon/version resource...
if exist resource.rc (
    windres resource.rc -O coff -o build\resource.res
    if errorlevel 1 ( echo   WARN: resource compile failed - continuing without icon ) else ( echo     OK: build\resource.res )
) else (
    echo   SKIP: resource.rc not found
)

echo [2/5] Release build...
if exist build\resource.res (
    gcc %FLAGS% -O2 main.c %SRC% %PLUGINS% %CORE% build\resource.res^
     -o build\Lass.exe %LIBS% -mwindows -static-libgcc -static
) else (
    gcc %FLAGS% -O2 main.c %SRC% %PLUGINS% %CORE%^
     -o build\Lass.exe %LIBS% -mwindows -static-libgcc -static
)
if errorlevel 1 goto :fail
echo     OK: build\Lass.exe

echo [3/5] Debug build...
gcc %FLAGS% -O0 -g -DIDM_DEBUG main.c %SRC% %PLUGINS% %CORE%^
 -o build\Lass-debug.exe %LIBS% -mwindows -static-libgcc -static
if errorlevel 1 goto :fail
echo     OK: build\Lass-debug.exe

echo [4/5] Native messaging helper...
gcc -O2 -std=c11 helper/lass_helper.c -o build\lass_helper.exe^
 -lkernel32 -static-libgcc
if errorlevel 1 ( echo   WARN: helper build failed ) else ( echo     OK: build\lass_helper.exe )

echo [5/5] Tests...
REM ws2_32 فقط برای tests/test_httpd.c لازمه (سرور loopback داخلی
REM برای تست concurrency واقعی) — Lass.exe اصلی بهش نیازی نداره.
gcc %FLAGS% -O0 -g %SRC% %PLUGINS% %CORE%^
 tests/test_all.c tests/test_httpd.c^
 -o build\test_all.exe %LIBS% -lws2_32 -static-libgcc
if errorlevel 1 goto :fail
echo     OK: build\test_all.exe

echo.
echo ====== BUILD SUCCESSFUL ======
echo   17 plugins: smart_resume, dynamic_seg, cookie_jar, chmml
echo               speed_graph, clipboard, basket
echo               ftp/ftps, hls/m3u8, dash/mpd, rtmp
echo               proxy, http_auth, browser_ext
echo               notify, categories
echo.
pause & exit /b 0
:fail
echo BUILD FAILED.
pause & exit /b 1
