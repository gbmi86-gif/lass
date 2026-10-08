/* plugin_api.c  –  registry و dispatcher پلاگین‌ها */

#include "plugin_api.h"
#include "../src/lass_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================
   ایجاد و حذف registry
   ====================================================== */
IDM_PluginRegistry *plugin_registry_create(void) {
    IDM_PluginRegistry *reg = (IDM_PluginRegistry *)calloc(1, sizeof(*reg));
    return reg;
}

void plugin_registry_destroy(IDM_PluginRegistry *reg) {
    if (!reg) return;
    for (int i = 0; i < reg->count; i++) {
        if (reg->plugins[i] && reg->plugins[i]->on_destroy)
            reg->plugins[i]->on_destroy();
    }
    free(reg);
}

/* ======================================================
   attach / detach
   ====================================================== */
int plugin_attach(IDM_PluginRegistry *reg, IDM_PluginHooks *plugin) {
    if (!reg || !plugin) return -1;
    if (reg->count >= IDM_MAX_PLUGINS) {
        LOGE(LOG_MOD_PLUGIN_API,
             "registry FULL (%d/%d) - '%s' NOT attached! "
             "Increase IDM_MAX_PLUGINS in plugin_api.h",
             reg->count, IDM_MAX_PLUGINS,
             plugin->name ? plugin->name : "?");
        return -1;
    }

    /* بررسی نسخه API */
    if (plugin->api_version != IDM_PLUGIN_API_VERSION) {
        LOGE(LOG_MOD_PLUGIN_API, "'%s' has wrong API version %d (expected %d)",
             plugin->name ? plugin->name : "?",
             plugin->api_version, IDM_PLUGIN_API_VERSION);
        return -1;
    }

    /* جلوگیری از attach دوباره — دفاعی: قبلاً reg->plugins[i]->name
       رو بدون چک NULL به strcmp می‌داد؛ اگه یه پلاگین بدشکل بدون
       name attach شده بود (مثلاً باگ تو یه پلاگین دیگه)، این
       strcmp(NULL, ...) یه crash واقعی بود، نه یه امتناع تمیز. */
    for (int i = 0; i < reg->count; i++) {
        if (reg->plugins[i] && reg->plugins[i]->name && plugin->name &&
            strcmp(reg->plugins[i]->name, plugin->name) == 0)
            return 0; /* already attached */
    }

    if (plugin->on_init) {
        if (plugin->on_init() != 0) {
            LOGE(LOG_MOD_PLUGIN_API, "'%s' init failed",
                 plugin->name ? plugin->name : "?");
            return -1;
        }
    }

    reg->plugins[reg->count++] = plugin;
    LOGI(LOG_MOD_PLUGIN_API, "attached: %s v%s",
         plugin->name    ? plugin->name    : "?",
         plugin->version ? plugin->version : "?");
    return 0;
}

void plugin_detach(IDM_PluginRegistry *reg, const char *name) {
    if (!reg || !name) return;
    for (int i = 0; i < reg->count; i++) {
        if (reg->plugins[i] && reg->plugins[i]->name &&
            strcmp(reg->plugins[i]->name, name) == 0) {
            if (reg->plugins[i]->on_destroy)
                reg->plugins[i]->on_destroy();
            /* جابجایی باقی‌مانده */
            for (int j = i; j < reg->count - 1; j++)
                reg->plugins[j] = reg->plugins[j + 1];
            reg->plugins[--reg->count] = NULL;
            return;
        }
    }
}

/* ======================================================
   Dispatchers
   ====================================================== */
void plugin_fire_download_start(IDM_PluginRegistry *reg, IDM_Download *dl) {
    if (!reg || !dl) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_download_start)
            reg->plugins[i]->on_download_start(dl);
}

void plugin_fire_download_done(IDM_PluginRegistry *reg, IDM_Download *dl) {
    if (!reg || !dl) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_download_done)
            reg->plugins[i]->on_download_done(dl);
}

void plugin_fire_download_pause(IDM_PluginRegistry *reg, IDM_Download *dl) {
    if (!reg || !dl) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_download_pause)
            reg->plugins[i]->on_download_pause(dl);
}

void plugin_fire_download_fail(IDM_PluginRegistry *reg, IDM_Download *dl) {
    if (!reg || !dl) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_download_fail)
            reg->plugins[i]->on_download_fail(dl);
}

void plugin_fire_segment_progress(IDM_PluginRegistry *reg,
                                   IDM_Segment *seg, uint64_t n) {
    if (!reg || !seg) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_segment_progress)
            reg->plugins[i]->on_segment_progress(seg, n);
}

void plugin_fire_segment_done(IDM_PluginRegistry *reg, IDM_Segment *seg) {
    if (!reg || !seg) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_segment_done)
            reg->plugins[i]->on_segment_done(seg);
}

int plugin_collect_http_headers(IDM_PluginRegistry *reg, IDM_Download *dl,
                                 char *buf, int max_len) {
    if (!reg || !dl || !buf || max_len <= 0) return 0;
    int total = 0;
    for (int i = 0; i < reg->count; i++) {
        if (reg->plugins[i] && reg->plugins[i]->on_http_headers) {
            int n = reg->plugins[i]->on_http_headers(dl,
                                                      buf + total,
                                                      max_len - total - 1);
            if (n > 0) total += n;
        }
    }
    buf[total] = '\0';
    return total;
}

void plugin_fire_set_cookie(IDM_PluginRegistry *reg, IDM_Download *dl,
                             const char *host, const char *raw_set_cookie) {
    if (!reg || !host || !raw_set_cookie) return;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i] && reg->plugins[i]->on_set_cookie)
            reg->plugins[i]->on_set_cookie(dl, host, raw_set_cookie);
}
