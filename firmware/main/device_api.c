/**
 * 设备管理 HTTP（STA 模式，:80）
 *
 * 服务端在 Web 里输入设备 IP 后，会先 GET /api/v1/info 探测身份，
 * 再 POST /api/v1/config 下发看板地址、API 设置和备注名。
 */

#include "device_api.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_runtime.h"
#include "wifi_mgr.h"

#ifndef DASH_FW_VERSION
#define DASH_FW_VERSION "dev"
#endif

static const char *TAG = "devapi";
static httpd_handle_t s_server;

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = text ? httpd_resp_sendstr(req, text) : ESP_FAIL;
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_info(httpd_req_t *req)
{
    app_config_t cfg;
    app_runtime_snapshot(&cfg);
    char ip[16] = "";
    wifi_mgr_sta_ip(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(root, "id", cfg.device_id);
    cJSON_AddStringToObject(root, "name", cfg.device_name);
    cJSON_AddStringToObject(root, "profile", "320x480");
    cJSON_AddStringToObject(root, "fw", DASH_FW_VERSION);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ssid", cfg.wifi_ssid);
    cJSON_AddBoolToObject(root, "configured", cfg.dashboard_url[0] != '\0');
    cJSON_AddBoolToObject(root, "server_connected",
                          app_runtime_server_connected());
    return send_json(req, root);
}

static esp_err_t handle_config(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 1536) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }
    char *body = heap_caps_malloc(1536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return httpd_resp_send_500(req);
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, body + total, len - total);
        if (r <= 0) {
            heap_caps_free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        }
        total += r;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    app_config_t cfg;
    app_runtime_snapshot(&cfg);

    const char *v;
    if ((v = json_str(root, "dashboard_url"))) {
        copy_str(cfg.dashboard_url, sizeof(cfg.dashboard_url), v);
    }
    if ((v = json_str(root, "name"))) {
        copy_str(cfg.device_name, sizeof(cfg.device_name), v);
    }
    if ((v = json_str(root, "device_id")) && v[0] && !cfg.device_id[0]) {
        copy_str(cfg.device_id, sizeof(cfg.device_id), v);
    }
    cJSON *rot = cJSON_GetObjectItemCaseSensitive(root, "rot180");
    if (cJSON_IsBool(rot)) cfg.rot180 = cJSON_IsTrue(rot);

    cJSON *api = cJSON_GetObjectItemCaseSensitive(root, "api");
    if (cJSON_IsObject(api)) {
        if ((v = json_str(api, "url"))) {
            copy_str(cfg.server_url, sizeof(cfg.server_url), v);
        }
        if ((v = json_str(api, "key"))) {
            copy_str(cfg.api_key, sizeof(cfg.api_key), v);
        }
        if ((v = json_str(api, "provider")) && v[0]) {
            copy_str(cfg.provider, sizeof(cfg.provider), v);
        }
        if ((v = json_str(api, "host_agent"))) {
            copy_str(cfg.host_agent, sizeof(cfg.host_agent), v);
        }
    }
    config_normalize(&cfg);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "config push: server=%s name=%s api=%s provider=%s rot=%d",
             cfg.dashboard_url[0] ? cfg.dashboard_url : "(none)",
             cfg.device_name[0] ? cfg.device_name : "(none)",
             cfg.server_url[0] ? cfg.server_url : "(none)",
             cfg.provider, cfg.rot180 ? 1 : 0);

    if (!app_runtime_apply_config(&cfg)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "apply failed");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

static esp_err_t handle_reboot(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddBoolToObject(resp, "ok", true);
        send_json(req, resp);
    } else {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    app_config_t cfg;
    app_runtime_snapshot(&cfg);
    char ip[16] = "";
    wifi_mgr_sta_ip(ip, sizeof(ip));
    char buf[512];
    snprintf(buf, sizeof(buf),
             "ESP32-WIFI-Dashboard device\nid: %s\nname: %s\nip: %s\n"
             "server: %s\n",
             cfg.device_id, cfg.device_name[0] ? cfg.device_name : "-", ip,
             cfg.dashboard_url[0] ? cfg.dashboard_url : "(not configured)");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, buf);
}

esp_err_t device_api_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 6;
    config.max_open_sockets = 2;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "device httpd start failed");
        s_server = NULL;
        return ESP_FAIL;
    }

    httpd_uri_t info = { .uri = "/api/v1/info", .method = HTTP_GET,
                         .handler = handle_info };
    httpd_uri_t cfg = { .uri = "/api/v1/config", .method = HTTP_POST,
                        .handler = handle_config };
    httpd_uri_t reboot = { .uri = "/api/v1/reboot", .method = HTTP_POST,
                           .handler = handle_reboot };
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET,
                         .handler = handle_root };
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &cfg);
    httpd_register_uri_handler(s_server, &reboot);
    httpd_register_uri_handler(s_server, &root);
    ESP_LOGI(TAG, "device api ready on :80");
    return ESP_OK;
}

void device_api_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "device api stopped");
}

bool device_api_running(void)
{
    return s_server != NULL;
}
