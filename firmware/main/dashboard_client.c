#include "dashboard_client.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_lvgl_port.h"

#include "api_client.h"
#include "dash_metric_store.h"
#include "display.h"
#include "ui_dynamic.h"
#include "wifi_mgr.h"

static const char *TAG = "dashcli";

#define LAYOUT_BUF_SIZE 16384
#define WS_BUF_SIZE 8192

static char s_base[160];
static bool s_rot180;
static char s_subscribe[2048];
static char s_ws_uri[220];
static char *s_ws_buf;
static size_t s_ws_len;
static esp_websocket_client_handle_t s_ws;

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

static bool fetch_and_build(void)
{
    char *buf = heap_caps_malloc(LAYOUT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for layout");
        return false;
    }

    int len = api_get_path(s_base,
                           "/api/v1/layout?profile=320x480&resolved=1",
                           NULL, buf, LAYOUT_BUF_SIZE, 6000);
    if (len <= 0) {
        ESP_LOGW(TAG, "layout fetch failed (%d)", len);
        heap_caps_free(buf);
        return false;
    }

    s_subscribe[0] = '\0';
    bool ok = false;
    if (lvgl_port_lock(3000)) {
        display_set_rotation(s_rot180 ? 180 : 0);
        ok = ui_dynamic_build(buf, s_subscribe, sizeof(s_subscribe));
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock timeout for dynamic UI");
    }
    heap_caps_free(buf);

    if (ok) {
        ESP_LOGI(TAG, "dynamic UI ready (%d bytes subscribe)", len);
    }
    return ok;
}

static void handle_snapshot(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "snapshot JSON parse failed");
        return;
    }

    const char *type = NULL;
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type_item)) type = type_item->valuestring;

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
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "websocket connected");
        const char *sub = s_subscribe[0]
                              ? s_subscribe
                              : "{\"type\":\"subscribe\"}";
        int sent = esp_websocket_client_send_text(s_ws, sub, (int)strlen(sub),
                                                  pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "subscribe sent: %d bytes", sent);
        break;
    }
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
                handle_snapshot(s_ws_buf);
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

    while (!wifi_mgr_sta_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wi-Fi ready, dashboard URL %s", s_base);

    while (1) {
        if (!fetch_and_build()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        build_ws_uri();
        esp_websocket_client_config_t cfg = {
            .uri = s_ws_uri,
            .reconnect_timeout_ms = 3000,
            .network_timeout_ms = 5000,
            .buffer_size = 4096,
            .task_stack = 8192,
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
        break;
    }

    vTaskDelete(NULL);
}

void dashboard_client_start(const char *dashboard_url, bool rot180)
{
    if (!dashboard_url || !dashboard_url[0]) return;
    snprintf(s_base, sizeof(s_base), "%s", dashboard_url);
    s_rot180 = rot180;
    s_ws_len = 0;
    s_ws = NULL;
    s_ws_buf = heap_caps_malloc(WS_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ws_buf) {
        ESP_LOGE(TAG, "no PSRAM for websocket buffer");
        return;
    }
    xTaskCreate(dashboard_task, "dash_cli", 12288, NULL, 4, NULL);
}
