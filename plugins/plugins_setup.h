/* plugins_setup.h */
#ifndef IDM_PLUGINS_SETUP_H
#define IDM_PLUGINS_SETUP_H

#include "plugin_api.h"
#include "plugin_basket.h"
#include "plugin_clipboard.h"
#include "plugin_ftp.h"
#include "plugin_hls.h"
#include "plugin_proxy.h"
#include "plugin_http_auth.h"
#include "plugin_browser_ext.h"
#include "plugin_dash.h"
#include "plugin_notify.h"
#include "plugin_categories.h"
#include "plugin_rtmp.h"

IDM_PluginRegistry *plugins_init    (void);
void                plugins_shutdown(IDM_PluginRegistry *reg);

/* سیگنال «باید خاموش بشی» رو به همه‌ی پلاگین‌هایی که یه thread
   پس‌زمینه‌ی دائمی دارن (نه thread‌های یک‌باره‌ی per-task مثل rtmp/
   video_sniffer) می‌فرسته — بدون هیچ wait/close/free. باید *قبل*
   از shutdown_brake_wait_all در main.c صدا زده بشه، وگرنه اون wait
   برای thread‌هایی که هنوز اصلاً خبردار نشدن باید خارج بشن، همیشه
   بی‌فایده timeout می‌خوره (دقیقاً همون چیزی که در لاگ واقعی دیده
   شد: هر session، عیناً همین ۴ پلاگین + engine_scheduler). خود
   plugins_shutdown (join واقعی + free) طبق معمول بعد از wait صدا
   زده میشه. */
void                plugins_signal_shutdown(IDM_PluginRegistry *reg);

void plugins_on_download_start   (IDM_Download *dl);
void plugins_on_download_done    (IDM_Download *dl);
void plugins_on_download_pause   (IDM_Download *dl);
void plugins_on_download_fail    (IDM_Download *dl);
void plugins_on_segment_progress (IDM_Segment *seg, uint64_t n);
void plugins_collect_http_headers(IDM_Download *dl, char *buf, int max_len);

IDM_PluginHooks *plugins_get_cookie_jar   (void);
IDM_PluginHooks *plugins_get_smart_resume (void);
IDM_PluginHooks *plugins_get_dynamic_seg  (void);
IDM_PluginHooks *plugins_get_speed_graph  (void);
IDM_PluginHooks *plugins_get_clipboard    (void);
IDM_PluginHooks *plugins_get_basket       (void);
IDM_PluginHooks *plugins_get_ftp          (void);
IDM_PluginHooks *plugins_get_hls          (void);
IDM_PluginHooks *plugins_get_proxy        (void);
IDM_PluginHooks *plugins_get_auth         (void);
IDM_PluginHooks *plugins_get_browser_ext  (void);
IDM_PluginHooks *plugins_get_dash         (void);
IDM_PluginHooks *plugins_get_notify       (void);
IDM_PluginHooks *plugins_get_categories   (void);
IDM_PluginHooks *plugins_get_rtmp         (void);

void plugins_set_speed_graph_parent(HWND parent, IDM_Engine *engine);
void plugins_set_basket_params     (HWND parent, IDM_Engine *engine,
                                    Basket_DropCb on_drop, void *userdata);
void plugins_set_clipboard_params  (HWND owner, CB_UrlDetected on_url,
                                    void *userdata);
void plugins_set_browser_ext_cb    (ExtDownloadCb on_download, void *userdata);
void plugins_set_notify_params     (HWND tray_hwnd, UINT tray_id);
void plugins_set_ini_path          (const char *ini_path);

#endif /* IDM_PLUGINS_SETUP_H */
/* video_sniffer */
#include "plugin_video_sniffer.h"
IDM_PluginHooks *plugins_get_video_sniffer(void);
void plugins_set_video_sniffer_cb(VS_FoundCb on_found, void *userdata);

/* speed_scheduler */
#include "plugin_speed_scheduler.h"
IDM_PluginHooks *plugins_get_speed_scheduler(void);
void plugins_set_speed_scheduler_engine(IDM_Engine *engine);
