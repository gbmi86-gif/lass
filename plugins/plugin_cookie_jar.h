/* plugin_cookie_jar.h
   ─────────────────────────────────────────────────────────────
   پلاگین ۳: Cookie Jar
   - ذخیره/بازیابی cookie برای سایت‌های login-required
   - پشتیبانی از expire، domain matching
   - import از browser (Netscape format)
   - inject خودکار به HTTP header هر دانلود
   ─────────────────────────────────────────────────────────────
*/
#ifndef IDM_PLUGIN_COOKIE_JAR_H
#define IDM_PLUGIN_COOKIE_JAR_H

#include "plugin_api.h"

IDM_PluginHooks *cookie_jar_plugin_create(const char *cookie_file);
void             cookie_jar_plugin_destroy(IDM_PluginHooks *plugin);

/* ---- API مستقیم ---- */
void cookie_jar_set    (IDM_PluginHooks *p, const char *domain,
                        const char *name, const char *value, int max_age_sec);
void cookie_jar_import (IDM_PluginHooks *p, const char *netscape_file);
void cookie_jar_save   (IDM_PluginHooks *p);
void cookie_jar_clear  (IDM_PluginHooks *p);

#endif /* IDM_PLUGIN_COOKIE_JAR_H */
