/* plugin_smart_resume.h
   ─────────────────────────────────────────────────────────────
   پلاگین ۱: Smart Resume
   - fingerprint هوشمند (url_hash + size + etag)
   - تشخیص تغییر فایل روی سرور
   - ذخیره chunk-level progress
   - رمزگذاری ساده state file با XOR key
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_SMART_RESUME_H
#define IDM_PLUGIN_SMART_RESUME_H

#include "plugin_api.h"

/* ---- ساخت پلاگین ---- */
IDM_PluginHooks *smart_resume_plugin_create(const char *state_dir,
                                             const uint8_t xor_key[8]);
void             smart_resume_plugin_destroy(IDM_PluginHooks *plugin);

/* ---- API مستقیم (برای GUI) ---- */
/* بررسی وجود resume برای یک URL */
bool smart_resume_exists(IDM_PluginHooks *plugin, const char *url,
                          const char *out_path);
/* حذف دستی state */
void smart_resume_delete(IDM_PluginHooks *plugin, const char *url,
                          const char *out_path);

#endif /* IDM_PLUGIN_SMART_RESUME_H */
