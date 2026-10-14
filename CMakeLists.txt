cmake_minimum_required(VERSION 3.16)
project(Lass C)
enable_language(RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_RC_COMPILER_INIT windres)
set(CMAKE_RC_COMPILE_OBJECT
    "<CMAKE_RC_COMPILER> -O coff <DEFINES> -i <SOURCE> -o <OBJECT>")
add_compile_options(-Wall -Wextra -DWIN32 -D_WIN32 -DWIN32_LEAN_AND_MEAN)
# در build نوع Debug (cmake -DCMAKE_BUILD_TYPE=Debug ..)، IDM_DEBUG
# ست میشه — همون قراردادی که build.bat هم برای هدف debug استفاده
# می‌کنه. این پلاگین CHMML (leak detector) رو فعال می‌کنه.
add_compile_definitions($<$<CONFIG:Debug>:IDM_DEBUG>)

set(SRC_FILES
    src/engine.c src/segment.c src/http.c
    src/storage.c src/resume.c src/settings.c src/queue_persist.c
    src/lass_log.c
    src/secure_store.c
    src/shutdown_brake.c
    src/checksum.c
    src/crash_report.c
    src/legacy_data.c
    src/i18n.c)

set(PLUGIN_FILES
    plugins/plugin_api.c plugins/plugins_setup.c
    plugins/plugin_smart_resume.c plugins/plugin_dynamic_seg.c
    plugins/plugin_cookie_jar.c plugins/plugin_chmml.c
    plugins/plugin_speed_graph.c
    plugins/plugin_clipboard.c
    plugins/plugin_basket.c
    plugins/plugin_ftp.c
    plugins/plugin_hls.c
    plugins/plugin_proxy.c
    plugins/plugin_http_auth.c
    plugins/plugin_browser_ext.c
    plugins/plugin_dash.c
    plugins/plugin_notify.c
    plugins/plugin_categories.c
    plugins/plugin_rtmp.c
    plugins/plugin_video_sniffer.c
    plugins/plugin_speed_scheduler.c)

set(CORE_FILES
    core/core_ldm.c
    core/guo.c)

add_library(idm_lib STATIC
    ${SRC_FILES} ${PLUGIN_FILES} ${CORE_FILES})
target_include_directories(idm_lib PUBLIC src plugins core)

add_executable(Lass WIN32 main.c resource.rc)
target_link_libraries(Lass PRIVATE idm_lib
    wininet comctl32 comdlg32 shell32 user32 gdi32 ole32 uuid shlwapi crypt32)
# قبلاً هیچ static linking‌ای در مسیر CMake (که مسیر build واقعی
# پروژه‌ست) وجود نداشت — یعنی Lass.exe به DLLهای runtime خودِ
# MinGW (libgcc_s_seh-1.dll و غیره) وابسته بود و روی هر سیستم
# ویندوزی دیگه‌ای که MinGW نصب نداشت اجرا نمی‌شد. -static همه‌ی
# این وابستگی‌ها رو داخل خودِ exe جاسازی می‌کنه — یه فایل واحد و
# مستقل که روی هر ویندوزی (بدون نیاز به نصب چیز اضافه) اجرا می‌شه.
target_link_options(Lass PRIVATE -static)

enable_testing()
add_executable(test_all tests/test_all.c tests/test_httpd.c)
target_link_libraries(test_all PRIVATE idm_lib
    wininet ws2_32 comctl32 comdlg32 shell32 user32 gdi32 ole32 uuid shlwapi crypt32)
target_include_directories(test_all PUBLIC src plugins core)
add_test(NAME full_test COMMAND test_all)
