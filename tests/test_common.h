#ifndef LASS_TEST_COMMON_H
#define LASS_TEST_COMMON_H

#include <winsock2.h>
/* test_all.c
   ─────────────────────────────────────────────────────────────
   Lass Download Manager – Test Suite  v5.1
   کامپایل:
     gcc -std=c11 -DWIN32 -D_WIN32 -DWIN32_LEAN_AND_MEAN
         -I src -I plugins -I core
         tests/test_all.c
         plugins/plugin_api.c plugins/plugin_chmml.c
         plugins/plugin_smart_resume.c plugins/plugin_dynamic_seg.c
         plugins/plugin_speed_graph.c
         plugins/plugin_clipboard.c plugins/plugin_basket.c
         plugins/plugin_ftp.c plugins/plugin_hls.c
         plugins/plugin_proxy.c plugins/plugin_http_auth.c
         plugins/plugin_browser_ext.c
         plugins/plugin_dash.c
         plugins/plugin_notify.c
         plugins/plugin_categories.c
         src/engine.c src/segment.c src/http.c
         src/storage.c src/resume.c src/settings.c
         core/core_ldm.c core/guo.c
         -o build/test_all.exe
         -lwininet -lcomctl32 -lcomdlg32 -lshell32
         -luser32 -lgdi32 -lole32 -luuid -lshlwapi
         -static-libgcc
   ─────────────────────────────────────────────────────────────
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "security_fixes.h"
#include "plugin_chmml.h"
#include "plugin_api.h"
#include "plugin_smart_resume.h"
#include "plugin_dynamic_seg.h"
#include "plugin_cookie_jar.h"
#include "plugin_speed_graph.h"
#include "plugin_clipboard.h"
#include "plugin_basket.h"
#include "plugin_ftp.h"
#include "plugin_hls.h"
#include "test_hls_server.h"
#include "plugin_dash.h"
#include "mp4_mux.h"
#include "dash_download.h"
#include "resume.h"
#include "../src/hls_download.h"
#include "plugin_proxy.h"
#include "plugin_http_auth.h"
#include "plugin_browser_ext.h"
#include "plugin_dash.h"
#include "plugin_notify.h"
#include "plugin_categories.h"
#include "plugin_rtmp.h"
#include "plugin_video_sniffer.h"
#include "../src/engine.h"
#include "../plugins/plugins_setup.h"
#include "../src/crash_report.h"
#include "../src/legacy_data.h"
#include "../src/transport.h"
#include "../src/url_secure.h"
#include "../src/tels_container.h"
#include "../plugins/plugin_ftp.h"
#include "test_ftpd.h"
#include "../src/queue_persist.h"
#include "../src/lass_store.h"
#include "../src/settings.h"
#include "../src/i18n.h"
#include "../src/http.h"
#include "../src/secure_store.h"
#include "../src/shutdown_brake.h"
#include "../src/lass_log.h"
#include "test_httpd.h"

/* ======================================================
   ابزار تست
   ====================================================== */

/* counters defined in test_common.c */
extern int g_pass;
extern int g_fail;
extern int g_total;

#define TEST(name, cond) do { \
    g_total++; \
    log_console_lock(); \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s  (line %d)\n", name, __LINE__); \
        g_fail++; \
    } \
    log_console_unlock(); \
} while(0)

#define SECTION(title) do { \
    log_console_lock(); \
    printf("\n=== %s ===\n", title); \
    log_console_unlock(); \
} while(0)

#define DT_CLEAN_NAME(n) do { char _cp[200]; DeleteFileA(n); \
    snprintf(_cp, sizeof(_cp), "%s%s", (n), IDM_STATE_FILE_EXT); DeleteFileA(_cp); \
    snprintf(_cp, sizeof(_cp), "%s.tels", (n)); DeleteFileA(_cp); } while (0)

/* test entry points (called from test_all.c) */
void test_security_fixes(void);
void test_chmml(void);
void test_plugin_registry(void);
void test_cookie_jar(void);
void test_smart_resume(void);
void test_dynamic_seg(void);
void test_speed_graph(void);
void test_clipboard(void);
void test_basket(void);
void test_ftp(void);
void test_hls(void);
void test_proxy(void);
void test_http_auth(void);
void test_engine_features(void);
void test_engine_fifo(void);
void test_engine_concurrent(void);
void test_engine_real_concurrency(void);
void test_server_negotiation(void);
void test_batch_url_extraction(void);
void test_legacy_migration(void);
void test_dl_lifecycle(void);
void test_ftp_download(void);
void test_url_credentials(void);
void test_tels_container(void);
void test_tels_encryption(void);
void test_tels_api_edges(void);
void test_lass_store(void);
void test_state_migration(void);
void test_engine_destroy_while_launching(void);
void test_engine_action_during_launch(void);
void test_segment_policy_parks_stuck_range(void);
void test_segment_honours_retry_after(void);
void test_crash_report(void);
void test_launch_retry_backoff(void);
void test_permanent_vs_transient_errors(void);
void test_http_extract_host(void);
void test_tels_temp_file(void);
void test_tels_long_path(void);
void test_queue_persist(void);
void test_http_fixup(void);
void test_filename_security(void);
void test_secure_store(void);
void test_command_injection_protection(void);
void test_content_length_validation(void);
void test_shutdown_brake(void);
void test_range_safety(void);
void test_settings(void);
void test_i18n(void);
void test_browser_ext(void);
void test_dash(void);
void test_notify(void);
void test_categories(void);
void test_rtmp(void);
void test_video_sniffer(void);
void test_tels_compaction(void);
void test_store_compaction(void);
void test_hls_parser(void);
void test_hls_plain(void);
void test_hls_aes(void);
void test_hls_fmp4(void);
void test_hls_byterange(void);
void test_hls_resume(void);
void test_hls_stop_resume(void);
void test_hls_rejects(void);
void test_hls_engine(void);
void test_hls_engine_failure(void);
void test_dash_media(void);
void test_resume_and_zero_byte(void);
void test_hls_all(void);


/* shared helpers (defined in their topic module) */
BOOL bytes_contain(const char *data, size_t n, const char *needle);
BOOL test_read_file(const char *path, uint8_t **out, size_t *len);
/* removes leftover test state files (*test*.idmstate) from earlier runs */
void test_sweep_stale_state(void);
#endif /* LASS_TEST_COMMON_H */
