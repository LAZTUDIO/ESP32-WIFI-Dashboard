#include "dashboard_client.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_lvgl_port.h"

#include "api_client.h"
#include "dash_metric_store.h"
#include "display.h"
#include "ui.h"
#include "ui_dynamic.h"
#include "wifi_mgr.h"

static const char *TAG = "dashcli";

#define LAYOUT_BUF_SIZE 16384
#define WS_BUF_SIZE 8192
#define CACHE_PATH "/spiffs/layout.json"
#define CACHE_TMP "/spiffs/layout.json.tmp"

/* 原子换屏要求新旧两份 UI 同时驻留；组件多时退回先删后建，避免 OOM */
#define ATOMIC_MAX_WIDGETS 10
#define ATOMIC_MIN_FREE_INTERNAL (50 * 1024)

static char s_base[160];
static bool s_rot180;
static char s_subscribe[2048];
static char s_ws_uri[220];
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
    const char *rest = s_base;
    const char *scheme = "ws://";
    if (strncmp(rest, "https://", 8) == 0) {
        scheme = "wss://";
        rest += 8;
    } else if (strncmp(rest, "http://", 7) == 0) {
        rest += 7;
    }
    size_t len = strlen(rest);
    while (len > 0 && rest[len - 1] == '/') len--;
    snprintf(s_ws_uri, sizeof(s_ws_uri), "%s%.*s/ws", scheme, (int)len, rest);
}

static void send_subscribe(void)
{
    if (!s_ws) return;
    const char *sub = s_subscribe[0] ? s_subscribe : "{\"type\":\"subscribe\"}";
    int sent = esp_websocket_client_send_text(s_ws, sub, (int)strlen(sub),
                                              pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "subscribe sent: %d bytes", sent);
}

static uint32_t s_layout_hash;
static bool s_layout_ready;

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
    int64_t t_start = esp_timer_get_time();
    char *buf = heap_caps_malloc(LAYOUT_BUF_SIZE,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for layout");
        return false;
    }

    int len = api_get_path(s_base,
                           "/api/v1/layout?profile=320x480&resolved=1",
                           NULL, buf, LAYOUT_BUF_SIZE, 6000);
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
        display_set_rotation(s_rot180 ? 180 : 0);
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
        if (ok) {
            ui_dynamic_commit();
            dashboard_ui_destroy();
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
            if (lvgl_port_lock(300)) {
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
        send_subscribe();
        if (s_refresh_q) {
            uint32_t token = 1;
            xQueueSend(s_refresh_q, &token, 0);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
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
    ESP_LOGI(TAG, "Wi-Fi ready (%s), dashboard URL %s", ip, s_base);

    bool built = false;
    bool ws_started = false;
    while (1) {
        if (!built) {
            if (!fetch_and_build(true)) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            built = true;
        }

        if (!ws_started) {
            build_ws_uri();
            esp_websocket_client_config_t cfg = {
                .uri = s_ws_uri,
                .reconnect_timeout_ms = 3000,
                .network_timeout_ms = 5000,
                .buffer_size = 4096,
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

        uint32_t token = 0;
        if (xQueueReceive(s_refresh_q, &token, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "refreshing layout from server");
            if (fetch_and_build(false) &&
                esp_websocket_client_is_connected(s_ws)) {
                send_subscribe();
            }
        }
    }
}

void dashboard_client_start(const char *dashboard_url, bool rot180)
{
    if (!dashboard_url || !dashboard_url[0]) return;
    snprintf(s_base, sizeof(s_base), "%s", dashboard_url);
    s_rot180 = rot180;
    s_ws_len = 0;
    s_ws = NULL;
    s_refresh_q = xQueueCreate(2, sizeof(uint32_t));
    s_ws_buf = heap_caps_malloc(WS_BUF_SIZE,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ws_buf || !s_refresh_q) {
        ESP_LOGE(TAG, "no memory for dashboard client");
        return;
    }
    xTaskCreate(dashboard_task, "dash_cli", 12288, NULL, 4, NULL);
}
