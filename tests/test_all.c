#include "test_common.h"

int main(void) {
    atexit(test_sweep_stale_state);
    test_sweep_stale_state();
    SetConsoleOutputCP(CP_UTF8);   /* خروجی فارسی و خط تیره درست نمایش داده شود */
    /* داده‌ی تست جدا از داده‌ی واقعی کاربر است: ini، cookies، فروشگاه، رجیستری و استارت به پوشه‌ی موقت می‌روند */
    {
        char tmp[MAX_PATH] = {0};
        char home[MAX_PATH] = {0};
        if (GetTempPathA(sizeof(tmp), tmp) > 0) {
            size_t tl = strlen(tmp);
            if (tl > 0 && tmp[tl - 1] == '\\') tl--;
            const char *sub = "lass_test_data";
            size_t sbl = strlen(sub);
            if (tl + 1 + sbl < sizeof(home)) {
                memcpy(home, tmp, tl);
                home[tl] = '\\';
                memcpy(home + tl + 1, sub, sbl + 1);
                CreateDirectoryA(home, NULL);
                SetEnvironmentVariableA("LASS_DATA_DIR", home);
            }
        }
    }
    printf("==========================================\n");
    printf(" Lass Download Manager – Test Suite v5\n");
    printf("==========================================\n");

    test_security_fixes();
    test_chmml();
    test_plugin_registry();
    test_cookie_jar();
    test_smart_resume();
    test_dynamic_seg();
    test_speed_graph();
    test_clipboard();
    test_basket();
    test_ftp();
    test_hls();
    test_hls_all();
    test_proxy();
    test_http_auth();
    test_engine_features();
    test_engine_fifo();
    test_engine_concurrent();
    test_engine_real_concurrency();
    test_server_negotiation();
    test_batch_url_extraction();
    test_crash_report();
    test_legacy_migration();
    test_dl_lifecycle();
    test_ftp_download();
    test_url_credentials();
    test_tels_container();
    test_tels_encryption();
    test_tels_api_edges();
    test_lass_store();
    test_store_compaction();
    test_state_migration();
    test_engine_destroy_while_launching();
    test_engine_action_during_launch();
    test_segment_policy_parks_stuck_range();
    test_segment_honours_retry_after();
    test_launch_retry_backoff();
    test_permanent_vs_transient_errors();
    test_http_extract_host();
    test_tels_temp_file();
    test_tels_long_path();
    test_tels_compaction();
    test_queue_persist();
    test_http_fixup();
    test_filename_security();
    test_secure_store();
    test_command_injection_protection();
    test_content_length_validation();
    test_shutdown_brake();
    test_range_safety();
    test_settings();
    test_i18n();
    test_browser_ext();
    test_dash();
    test_dash_media();
    test_resume_and_zero_byte();
    test_notify();
    test_categories();
    test_rtmp();
    test_video_sniffer();

    printf("\n==========================================\n");
    printf(" Results: %d/%d passed", g_pass, g_total);
    if (g_fail > 0)
        printf("  |  %d FAILED", g_fail);
    printf("\n==========================================\n");
    fflush(stdout);

    /* از ExitProcess به‌جای return استفاده می‌کنیم تا از crash بعد از
       نتیجه جلوگیری کنیم. این crash از MinGW CRT cleanup (atexit، thread
       TLS destructors) میاد که با launch_thread های background که هنوز
       در حال HTTP timeout هستن تداخل پیدا می‌کنه. این مشکل فقط در
       test binary هست — در Lass.exe اصلی که event loop داره و شاتداون
       مرتب انجام میشه، این مشکل وجود نداره. ExitProcess مستقیماً
       به Windows kernel میره و از CRT teardown کاملاً رد میشه. */
    ExitProcess((g_fail == 0) ? 0 : 1);
}
