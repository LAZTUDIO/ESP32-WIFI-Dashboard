#include "dashboard_client.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_lvgl_port.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "api_client.h"
#include "app_runtime.h"
#include "dash_metric_store.h"
#include "display.h"
#include "metrics.h"
#include "ui_dynamic.h"
#include "wifi_mgr.h"

static const char *TAG = "dashcli";

#define LAYOUT_BUF_SIZE 16384
#define WS_BUF_SIZE 8192
#define CACHE_PATH "/spiffs/layout.json"
#define CACHE_TMP "/spiffs/layout.json.tmp"

/* Web 设计使用 11/13/15 三档中文字号；每档双槽轮换，
 * 避免覆盖正在被 LVGL 打开的文件 */
#define FONT_SIZE_COUNT 3
#define FONT_MAX_BYTES (256 * 1024)
static const int k_font_sizes[FONT_SIZE_COUNT] = {11, 13, 15};

/* 原子换屏要求新旧两份 UI 同时驻留；组件多时退回先删后建，避免 OOM */
#define ATOMIC_MAX_WIDGETS 10
/* 在线时管理 HTTP 已释放，内部 RAM 约 39KB；原子构建失败会自动回退先删后建。 */
#define ATOMIC_MIN_FREE_INTERNAL (32 * 1024)

static char s_base[160];
static char s_device_id[24];
static bool s_rot180;
static SemaphoreHandle_t s_client_lock;
static volatile bool s_reload_requested;
static bool s_client_started;

#define CLIENT_RELOAD_TOKEN 0xFFFFFFFFu

static void client_snapshot(char *base, size_t cap, char *device_id,
                            size_t id_cap, bool *rot)
{
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (base) snprintf(base, cap, "%s", s_base);
    if (device_id) snprintf(device_id, id_cap, "%s", s_device_id);
    if (rot) *rot = s_rot180;
    if (s_client_lock) xSemaphoreGive(s_client_lock);
}
static char s_subscribe[2048];
static char s_ws_uri[256];
static char *s_ws_buf;
static size_t s_ws_len;
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_refresh_q;

static void mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS ready: %u/%u bytes used",
                 (unsigned)used, (unsigned)total);
    }
}

static bool cache_write(const char *data, size_t len)
{
    FILE *f = fopen(CACHE_TMP, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        remove(CACHE_TMP);
        return false;
    }
    remove(CACHE_PATH);
    if (rename(CACHE_TMP, CACHE_PATH) != 0) {
        ESP_LOGW(TAG, "layout cache rename failed");
        return false;
    }
    ESP_LOGI(TAG, "layout cached: %u bytes", (unsigned)len);
    return true;
}

static char *cache_read(size_t *out_len)
{
    FILE *f = fopen(CACHE_PATH, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size >= LAYOUT_BUF_SIZE) {
        fclose(f);
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)size + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        heap_caps_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

static void build_ws_uri(void)
{
    char base[160], device_id[24];
    client_snapshot(base, sizeof(base), device_id, sizeof(device_id), NULL);
    s_ws_uri[0] = '\0';
    if (!base[0] || !device_id[0]) return;
    const char *rest = base;
    const char *scheme = "ws://";
    if (strncmp(rest, "https://", 8) == 0) {
        scheme = "wss://";
        rest += 8;
    } else if (strncmp(rest, "http://", 7) == 0) {
        rest += 7;
    }
    size_t len = strlen(rest);
    while (len > 0 && rest[len - 1] == '/') len--;
    snprintf(s_ws_uri, sizeof(s_ws_uri), "%s%.*s/ws?device_id=%s", scheme,
             (int)len, rest, device_id);
}

static void send_subscribe(void)
{
    if (!s_ws) return;
    const char *sub = s_subscribe[0] ? s_subscribe : "{\"type\":\"subscribe\"}";
    int sent = esp_websocket_client_send_text(s_ws, sub, (int)strlen(sub),
                                              pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "subscribe sent: %d bytes", sent);
}

/** 把屏幕实际识别到的 API provider / 可用状态发给服务端，编辑器据此过滤测点。 */
static void send_status(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    app_config_t cfg;
    app_runtime_snapshot(&cfg);
    const char *provider = metrics_provider_key();
    char msg[192];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"status\",\"provider\":\"%s\",\"api_ok\":%s,"
             "\"api_configured\":%s}",
             provider, metrics_last_ok() ? "true" : "false",
             cfg.server_url[0] ? "true" : "false");
    int sent = esp_websocket_client_send_text(s_ws, msg, (int)strlen(msg),
                                              pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "status sent: %d bytes provider=%s api_ok=%d", sent,
             provider, metrics_last_ok() ? 1 : 0);
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void json_copy_str(cJSON *obj, const char *key, char *dst, size_t cap)
{
    const char *value = json_str(obj, key);
    if (value) snprintf(dst, cap, "%s", value);
}

static bool config_same(const app_config_t *a, const app_config_t *b)
{
    return strcmp(a->dashboard_url, b->dashboard_url) == 0 &&
           strcmp(a->device_name, b->device_name) == 0 &&
           strcmp(a->server_url, b->server_url) == 0 &&
           strcmp(a->api_key, b->api_key) == 0 &&
           strcmp(a->provider, b->provider) == 0 &&
           strcmp(a->host_agent, b->host_agent) == 0 &&
           a->rot180 == b->rot180;
}

/** WS 下发的配置（服务端在设备上线后同步）；字段与服务端 /config 一致。 */
static void apply_ws_config(cJSON *node)
{
    app_config_t next;
    app_runtime_snapshot(&next);
    json_copy_str(node, "dashboard_url", next.dashboard_url,
                  sizeof(next.dashboard_url));
    json_copy_str(node, "name", next.device_name, sizeof(next.device_name));
    cJSON *api = cJSON_GetObjectItemCaseSensitive(node, "api");
    if (cJSON_IsObject(api)) {
        json_copy_str(api, "url", next.server_url, sizeof(next.server_url));
        json_copy_str(api, "key", next.api_key, sizeof(next.api_key));
        json_copy_str(api, "provider", next.provider, sizeof(next.provider));
        json_copy_str(api, "host_agent", next.host_agent,
                      sizeof(next.host_agent));
    }
    cJSON *rot = cJSON_GetObjectItemCaseSensitive(node, "rot180");
    if (cJSON_IsBool(rot)) next.rot180 = cJSON_IsTrue(rot);
    config_normalize(&next);

    app_config_t cur;
    app_runtime_snapshot(&cur);
    if (config_same(&cur, &next)) return;
    ESP_LOGI(TAG, "ws config applied: name=%s api=%s",
             next.device_name[0] ? next.device_name : "(none)",
             next.server_url[0] ? next.server_url : "(none)");
    app_runtime_apply_config(&next);
}

static uint32_t s_layout_hash;
static bool s_layout_ready;

/* ============================ 运行时中文字体 ============================ */

static lv_font_t *s_fonts[FONT_SIZE_COUNT];
static lv_font_t *s_old_fonts[FONT_SIZE_COUNT];
static char s_old_paths[FONT_SIZE_COUNT][32];

static void font_slot_path(int idx, uint8_t slot, char *out, size_t cap)
{
    snprintf(out, cap, "/spiffs/font_%d_%c.bin", k_font_sizes[idx],
             slot ? 'b' : 'a');
}

static void font_lv_path(int idx, uint8_t slot, char *out, size_t cap)
{
    snprintf(out, cap, "S:/font_%d_%c.bin", k_font_sizes[idx],
             slot ? 'b' : 'a');
}

static void nvs_font_key(int idx, const char *field, char *out, size_t cap)
{
    snprintf(out, cap, "f%d%s", k_font_sizes[idx], field);
}

static void nvs_font_info(int idx, char *sha, size_t cap, uint8_t *slot)
{
    sha[0] = '\0';
    *slot = 0;
    char key[16];
    nvs_handle_t h;
    if (nvs_open("llm_dash", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = cap;
    nvs_font_key(idx, "sha", key, sizeof(key));
    nvs_get_str(h, key, sha, &len);
    nvs_font_key(idx, "slot", key, sizeof(key));
    nvs_get_u8(h, key, slot);
    nvs_close(h);
}

static void nvs_font_save(int idx, const char *sha, uint8_t slot)
{
    char key[16];
    nvs_handle_t h;
    if (nvs_open("llm_dash", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_font_key(idx, "sha", key, sizeof(key));
    nvs_set_str(h, key, sha);
    nvs_font_key(idx, "slot", key, sizeof(key));
    nvs_set_u8(h, key, slot);
    nvs_commit(h);
    nvs_close(h);
}

/** 从指定槽加载 binfont；一次性把位图读进 RAM（每档约 8–20KB，走 PSRAM），
 *  避免渲染时再做 SPIFFS 读取，也避免快照等长渲染路径卡住 */
static lv_font_t *font_load_slot(int idx, uint8_t slot)
{
    char lv_path[32];
    font_lv_path(idx, slot, lv_path, sizeof(lv_path));
    lv_binfont_dsc_t dsc = {
        .path = lv_path,
        .dynamic_glyph_load = false,
    };
    lv_font_t *font = NULL;
    if (lvgl_port_lock(1500)) {
        font = lv_binfont_create_ex(&dsc);
        lvgl_port_unlock();
    }
    return font;
}

/** 切换某一档字体并记下旧字体，等界面重建完成后再释放（旧控件还在用它） */
static bool font_install(int idx, uint8_t slot, lv_font_t *font)
{
    if (!font) return false;
    if (!lvgl_port_lock(1500)) {
        lv_binfont_destroy(font);
        return false;
    }
    if (s_fonts[idx]) {
        s_old_fonts[idx] = s_fonts[idx];
        font_slot_path(idx, slot ? 0 : 1, s_old_paths[idx],
                       sizeof(s_old_paths[idx]));
    }
    s_fonts[idx] = font;
    ui_dynamic_set_cjk_fonts(s_fonts[0], s_fonts[1], s_fonts[2]);
    lvgl_port_unlock();
    return true;
}

/** 界面已重建（旧屏已删除）后调用；必须持有 LVGL 锁 */
static void font_release_old(void)
{
    for (int i = 0; i < FONT_SIZE_COUNT; i++) {
        if (!s_old_fonts[i]) continue;
        lv_binfont_destroy(s_old_fonts[i]);
        s_old_fonts[i] = NULL;
        if (s_old_paths[i][0]) {
            remove(s_old_paths[i]);
            s_old_paths[i][0] = '\0';
        }
    }
}

static void font_sync(void)
{
    char base[160], device_id[24];
    client_snapshot(base, sizeof(base), device_id, sizeof(device_id), NULL);
    if (!base[0] || !device_id[0]) return;

    char sha[FONT_SIZE_COUNT][80];
    uint8_t slot[FONT_SIZE_COUNT];
    for (int i = 0; i < FONT_SIZE_COUNT; i++) {
        nvs_font_info(i, sha[i], sizeof(sha[i]), &slot[i]);
    }

    /* 先用本地缓存，避免每次开机都重新下载 */
    for (int i = 0; i < FONT_SIZE_COUNT; i++) {
        if (s_fonts[i] || !sha[i][0]) continue;
        lv_font_t *cached = font_load_slot(i, slot[i]);
        if (cached) {
            if (font_install(i, slot[i], cached)) {
                char other[32];
                font_slot_path(i, slot[i] ? 0 : 1, other, sizeof(other));
                remove(other);
                ESP_LOGI(TAG, "font %d cache loaded: slot %u", k_font_sizes[i],
                         (unsigned)slot[i]);
            }
        } else {
            ESP_LOGW(TAG, "font %d cache load failed, will re-download",
                     k_font_sizes[i]);
        }
    }

    char meta[1536];
    char meta_path[160];
    snprintf(meta_path, sizeof(meta_path),
             "/api/v1/devices/%s/font/meta?profile=320x480", device_id);
    int n = api_get_path(base, meta_path, NULL, meta, sizeof(meta), 3000);
    if (n <= 0) {
        ESP_LOGI(TAG, "font meta unavailable (%d), keep builtin font", n);
        return;
    }
    cJSON *root = cJSON_Parse(meta);
    if (!root) {
        ESP_LOGW(TAG, "font meta JSON parse failed");
        return;
    }
    cJSON *sizes_item = cJSON_GetObjectItemCaseSensitive(root, "sizes");
    if (!cJSON_IsArray(sizes_item)) {
        ESP_LOGW(TAG, "font meta has no sizes array, keep builtin font");
        cJSON_Delete(root);
        return;
    }

    char *buf = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, sizes_item) {
        cJSON *size_item = cJSON_GetObjectItemCaseSensitive(item, "size");
        cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(item, "sha256");
        if (!cJSON_IsNumber(size_item) || !cJSON_IsString(sha_item) ||
            !sha_item->valuestring[0]) {
            continue;
        }
        int px = (int)size_item->valuedouble;
        int idx = -1;
        for (int i = 0; i < FONT_SIZE_COUNT; i++) {
            if (k_font_sizes[i] == px) {
                idx = i;
                break;
            }
        }
        if (idx < 0) continue;
        const char *remote_sha = sha_item->valuestring;
        if (s_fonts[idx] && strcasecmp(sha[idx], remote_sha) == 0) continue;

        if (!buf) {
            buf = heap_caps_malloc(FONT_MAX_BYTES + 1,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                ESP_LOGE(TAG, "no PSRAM for font download");
                break;
            }
        }
        char url[160];
        snprintf(url, sizeof(url),
                 "/api/v1/devices/%s/font?profile=320x480&size=%d", device_id,
                 px);
        int len = api_get_path(base, url, NULL, buf, FONT_MAX_BYTES + 1,
                               10000);
        if (len <= 0) {
            ESP_LOGW(TAG, "font %d download failed (%d)", px, len);
            continue;
        }

        unsigned char digest[32];
        mbedtls_sha256((const unsigned char *)buf, (size_t)len, digest, 0);
        char hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        if (strcasecmp(hex, remote_sha) != 0) {
            ESP_LOGE(TAG, "font %d sha mismatch: %s != %s", px, hex, remote_sha);
            continue;
        }

        uint8_t new_slot = s_fonts[idx] ? (slot[idx] ? 0 : 1) : 0;
        char path[32];
        font_slot_path(idx, new_slot, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "font open failed: %s", path);
            continue;
        }
        size_t written = fwrite(buf, 1, (size_t)len, f);
        fclose(f);
        if (written != (size_t)len) {
            ESP_LOGE(TAG, "font %d write short: %u/%u", px, (unsigned)written,
                     (unsigned)len);
            remove(path);
            continue;
        }

        lv_font_t *font = font_load_slot(idx, new_slot);
        if (!font) {
            ESP_LOGE(TAG, "binfont %d load failed: %s", px, path);
            remove(path);
            continue;
        }
        if (!font_install(idx, new_slot, font)) {
            continue;
        }
        slot[idx] = new_slot;
        snprintf(sha[idx], sizeof(sha[idx]), "%s", remote_sha);
        nvs_font_save(idx, remote_sha, new_slot);
        ESP_LOGI(TAG, "font %d updated: slot %u, %d bytes", px,
                 (unsigned)new_slot, len);
    }
    if (buf) heap_caps_free(buf);
    cJSON_Delete(root);
}

static uint32_t layout_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static bool fetch_and_build(bool allow_cache)
{
    char base[160], device_id[24];
    bool rot = false;
    client_snapshot(base, sizeof(base), device_id, sizeof(device_id), &rot);
    if (!base[0] || !device_id[0]) return false;

    int64_t t_start = esp_timer_get_time();
    char *buf = heap_caps_malloc(LAYOUT_BUF_SIZE,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for layout");
        return false;
    }

    char layout_path[160];
    snprintf(layout_path, sizeof(layout_path),
             "/api/v1/devices/%s/layout?profile=320x480&resolved=1", device_id);
    int len = api_get_path(base, layout_path, NULL, buf, LAYOUT_BUF_SIZE, 6000);
    bool from_cache = false;
    if (len <= 0 && allow_cache) {
        size_t cached_len = 0;
        char *cached = cache_read(&cached_len);
        if (cached) {
            heap_caps_free(buf);
            buf = cached;
            len = (int)cached_len;
            from_cache = true;
            ESP_LOGW(TAG, "layout fetch failed, using cache");
        }
    }
    if (len <= 0) {
        ESP_LOGW(TAG, "layout fetch failed (%d)", len);
        heap_caps_free(buf);
        return false;
    }

    uint32_t hash = layout_hash(buf, (size_t)len);
    if (s_layout_ready && hash == s_layout_hash && ui_dynamic_active()) {
        if (!from_cache) cache_write(buf, (size_t)len);
        ESP_LOGI(TAG, "layout unchanged: %d bytes, %lld ms", len,
                 (long long)((esp_timer_get_time() - t_start) / 1000));
        heap_caps_free(buf);
        return true;
    }

    s_subscribe[0] = '\0';
    int next_widgets = ui_dynamic_json_widget_count(buf);
    bool ok = false;
    if (lvgl_port_lock(3000)) {
        display_set_rotation(rot ? 180 : 0);
        bool active = ui_dynamic_active();
        bool atomic = !active ||
                      (ui_dynamic_widget_count() <= ATOMIC_MAX_WIDGETS &&
                       next_widgets <= ATOMIC_MAX_WIDGETS &&
                       heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >
                           ATOMIC_MIN_FREE_INTERNAL);
        if (!atomic && active) {
            ui_dynamic_destroy();
        }
        ok = ui_dynamic_build(buf, s_subscribe, sizeof(s_subscribe));
        if (!ok && atomic && active) {
            /* 原子构建内存不够：丢掉旧屏，回退到先删后建，保证布局一定能更新 */
            ESP_LOGW(TAG, "atomic build failed, fallback to in-place rebuild");
            ui_dynamic_destroy();
            ok = ui_dynamic_build(buf, s_subscribe, sizeof(s_subscribe));
        }
        if (ok) {
            ui_dynamic_commit();
            font_release_old();
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock timeout for dynamic UI");
    }

    if (ok && !from_cache) {
        cache_write(buf, (size_t)len);
    }
    s_layout_hash = hash;
    s_layout_ready = ok;
    ESP_LOGI(TAG, "layout %s: %d bytes, subscribe=%u bytes, %lld ms",
             from_cache ? "cache" : "http", len,
             (unsigned)strlen(s_subscribe),
             (long long)((esp_timer_get_time() - t_start) / 1000));
    heap_caps_free(buf);
    return ok;
}

static void handle_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ws JSON parse failed");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : NULL;

    if (type && strcmp(type, "config") == 0) {
        cJSON *cfg = cJSON_GetObjectItemCaseSensitive(root, "config");
        if (cJSON_IsObject(cfg)) apply_ws_config(cfg);
        cJSON_Delete(root);
        return;
    }

    if (type && strcmp(type, "reboot") == 0) {
        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return;
    }

    if (type && strcmp(type, "layout_changed") == 0) {
        uint32_t token = 1;
        if (s_refresh_q) xQueueSend(s_refresh_q, &token, 0);
        ESP_LOGI(TAG, "layout_changed received");
        cJSON_Delete(root);
        return;
    }

    if (type && strcmp(type, "snapshot") == 0) {
        cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
        if (cJSON_IsObject(values)) {
            /* 与直连 API 任务共用 store：写入和刷屏都放在 LVGL 锁内串行化。 */
            if (lvgl_port_lock(300)) {
                for (cJSON *item = values->child; item; item = item->next) {
                    if (!item->string) continue;
                    cJSON *a = cJSON_GetObjectItemCaseSensitive(item, "a");
                    cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "v");
                    bool available = cJSON_IsBool(a) ? cJSON_IsTrue(a) : true;
                    if (!available || !v || cJSON_IsNull(v)) {
                        dash_metric_store_mark_unavailable(item->string);
                    } else if (cJSON_IsBool(v)) {
                        dash_metric_store_set_bool(item->string, cJSON_IsTrue(v), true);
                    } else if (cJSON_IsNumber(v)) {
                        dash_metric_store_set_number(item->string, v->valuedouble, true);
                    } else if (cJSON_IsString(v)) {
                        dash_metric_store_set_string(item->string, v->valuestring, true);
                    }
                }
                ui_dynamic_apply();
                lvgl_port_unlock();
            }
        }
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        app_runtime_set_server_connected(true);
        send_subscribe();
        send_status();
        if (s_refresh_q) {
            uint32_t token = 1;
            xQueueSend(s_refresh_q, &token, 0);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
        app_runtime_set_server_connected(false);
        s_ws_len = 0;
        break;
    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        if (!d || !s_ws_buf) break;
        if (d->op_code != 0x1 && d->op_code != 0x0) break;
        if (d->payload_offset == 0) s_ws_len = 0;
        if (s_ws_len + (size_t)d->data_len < WS_BUF_SIZE) {
            memcpy(s_ws_buf + s_ws_len, d->data_ptr, d->data_len);
            s_ws_len += (size_t)d->data_len;
        } else {
            ESP_LOGW(TAG, "ws frame too large, dropped");
            s_ws_len = 0;
            break;
        }
        if (d->fin || d->payload_offset + d->data_len >= d->payload_len) {
            if (s_ws_len > 0 && s_ws_len < WS_BUF_SIZE) {
                s_ws_buf[s_ws_len] = '\0';
                handle_message(s_ws_buf);
            }
            s_ws_len = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error");
        break;
    default:
        break;
    }
}

static void ws_stop(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_len = 0;
}

static void dashboard_task(void *arg)
{
    (void)arg;

    if (!dash_metric_store_init()) {
        ESP_LOGE(TAG, "metric store init failed");
        vTaskDelete(NULL);
        return;
    }
    mount_storage();

    char ip[16];
    while (!wifi_mgr_sta_ip(ip, sizeof(ip))) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wi-Fi ready (%s)", ip);

    bool built = false;
    bool ws_started = false;
    while (1) {
        char base[160], device_id[24];
        bool rot = false;
        client_snapshot(base, sizeof(base), device_id, sizeof(device_id), &rot);

        if (s_reload_requested) {
            s_reload_requested = false;
            ws_stop();
            built = false;
            ws_started = false;
            s_layout_ready = false;
            s_layout_hash = 0;
            ESP_LOGI(TAG, "dashboard reload: %s device=%s", base, device_id);
            continue;
        }

        if (!base[0] || !device_id[0]) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!built) {
            font_sync();
            if (!fetch_and_build(true)) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            built = true;
        }

        if (!ws_started) {
            build_ws_uri();
            if (!s_ws_uri[0]) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            esp_websocket_client_config_t cfg = {
                .uri = s_ws_uri,
                .reconnect_timeout_ms = 3000,
                .network_timeout_ms = 5000,
                .buffer_size = 2048,
                .task_stack = 6144,
            };
            s_ws = esp_websocket_client_init(&cfg);
            if (!s_ws) {
                ESP_LOGE(TAG, "websocket init failed");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                          ws_event_handler, NULL);
            if (esp_websocket_client_start(s_ws) != ESP_OK) {
                ESP_LOGE(TAG, "websocket start failed");
                esp_websocket_client_destroy(s_ws);
                s_ws = NULL;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            ESP_LOGI(TAG, "websocket started: %s", s_ws_uri);
            ws_started = true;
        }

        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            static char last_provider[20];
            static int last_api_ok = -1;
            const char *provider = metrics_provider_name();
            int api_ok = metrics_last_ok() ? 1 : 0;
            if (strcmp(provider, last_provider) != 0 || api_ok != last_api_ok) {
                send_status();
                snprintf(last_provider, sizeof(last_provider), "%s", provider);
                last_api_ok = api_ok;
            }
        }

        uint32_t token = 0;
        if (xQueueReceive(s_refresh_q, &token, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_reload_requested) continue;
            ESP_LOGI(TAG, "refreshing layout from server");
            font_sync();
            if (fetch_and_build(false) && s_ws &&
                esp_websocket_client_is_connected(s_ws)) {
                send_subscribe();
            }
        }
    }
}

void dashboard_client_start(const char *dashboard_url, const char *device_id,
                            bool rot180)
{
    if (s_client_started) {
        dashboard_client_reconfigure(dashboard_url, device_id, rot180);
        return;
    }
    if (!s_client_lock) s_client_lock = xSemaphoreCreateMutex();
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    snprintf(s_base, sizeof(s_base), "%s", dashboard_url ? dashboard_url : "");
    snprintf(s_device_id, sizeof(s_device_id), "%s", device_id ? device_id : "");
    s_rot180 = rot180;
    if (s_client_lock) xSemaphoreGive(s_client_lock);

    s_client_started = true;
    s_ws_len = 0;
    s_ws = NULL;
    if (!s_refresh_q) s_refresh_q = xQueueCreate(4, sizeof(uint32_t));
    if (!s_ws_buf) {
        s_ws_buf = heap_caps_malloc(WS_BUF_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_ws_buf || !s_refresh_q) {
        ESP_LOGE(TAG, "no memory for dashboard client");
        s_client_started = false;
        return;
    }
    xTaskCreate(dashboard_task, "dash_cli", 12288, NULL, 4, NULL);
}

void dashboard_client_reconfigure(const char *dashboard_url,
                                  const char *device_id, bool rot180)
{
    if (!s_client_started) {
        dashboard_client_start(dashboard_url, device_id, rot180);
        return;
    }
    if (!s_client_lock) s_client_lock = xSemaphoreCreateMutex();
    char old_base[160], old_id[24];
    client_snapshot(old_base, sizeof(old_base), old_id, sizeof(old_id), NULL);
    const char *url = dashboard_url ? dashboard_url : "";
    const char *id = device_id ? device_id : "";
    bool changed = strcmp(old_base, url) != 0 || strcmp(old_id, id) != 0;

    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    snprintf(s_base, sizeof(s_base), "%s", url);
    snprintf(s_device_id, sizeof(s_device_id), "%s", id);
    s_rot180 = rot180;
    if (s_client_lock) xSemaphoreGive(s_client_lock);

    if (changed) remove(CACHE_PATH);
    s_reload_requested = true;
    uint32_t token = CLIENT_RELOAD_TOKEN;
    if (s_refresh_q) xQueueSend(s_refresh_q, &token, pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "dashboard reconfigure: %s device=%s", url, id);
}
