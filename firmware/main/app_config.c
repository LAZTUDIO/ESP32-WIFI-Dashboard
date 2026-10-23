#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_config.h"

static const char *TAG = "cfg";
static const char *NS = "llm_dash";

void config_defaults(app_config_t *cfg)
{
    /* 开源版本不带任何服务器地址和密钥：首次上电一律进配网模式，由用户填写 */
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->provider, sizeof(cfg->provider), "auto");
}

void config_normalize(app_config_t *cfg)
{
    size_t n = strlen(cfg->server_url);
    while (n > 0 && cfg->server_url[n - 1] == '/') cfg->server_url[--n] = '\0';
    n = strlen(cfg->host_agent);
    while (n > 0 && cfg->host_agent[n - 1] == '/') cfg->host_agent[--n] = '\0';
    n = strlen(cfg->dashboard_url);
    while (n > 0 && cfg->dashboard_url[n - 1] == '/') cfg->dashboard_url[--n] = '\0';
}

void config_load(app_config_t *cfg)
{
    config_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no saved config, using defaults");
        return;
    }

    size_t len;
    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(h, "ssid", cfg->wifi_ssid, &len);
    len = sizeof(cfg->wifi_pass);
    nvs_get_str(h, "pass", cfg->wifi_pass, &len);
    len = sizeof(cfg->server_url);
    nvs_get_str(h, "server", cfg->server_url, &len);
    len = sizeof(cfg->api_key);
    nvs_get_str(h, "key", cfg->api_key, &len);
    len = sizeof(cfg->provider);
    nvs_get_str(h, "provider", cfg->provider, &len);
    len = sizeof(cfg->host_agent);
    nvs_get_str(h, "host", cfg->host_agent, &len);
    len = sizeof(cfg->dashboard_url);
    nvs_get_str(h, "dashboard", cfg->dashboard_url, &len);
    uint8_t rot = 0;
    if (nvs_get_u8(h, "rot180", &rot) == ESP_OK) cfg->rot180 = (rot != 0);
    nvs_close(h);

    config_normalize(cfg);
    ESP_LOGI(TAG, "loaded: wifi=%s api=%s dashboard=%s provider=%s",
             cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(none)",
             cfg->server_url[0] ? cfg->server_url : "(none)",
             cfg->dashboard_url[0] ? cfg->dashboard_url : "(none)",
             cfg->provider);
}

bool config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    bool ok = true;
    ok &= nvs_set_str(h, "ssid", cfg->wifi_ssid) == ESP_OK;
    ok &= nvs_set_str(h, "pass", cfg->wifi_pass) == ESP_OK;
    ok &= nvs_set_str(h, "server", cfg->server_url) == ESP_OK;
    ok &= nvs_set_str(h, "key", cfg->api_key) == ESP_OK;
    ok &= nvs_set_str(h, "provider", cfg->provider) == ESP_OK;
    ok &= nvs_set_str(h, "host", cfg->host_agent) == ESP_OK;
    ok &= nvs_set_str(h, "dashboard", cfg->dashboard_url) == ESP_OK;
    ok &= nvs_set_u8(h, "rot180", cfg->rot180 ? 1 : 0) == ESP_OK;
    ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);

    ESP_LOGI(TAG, "save %s", ok ? "ok" : "failed");
    return ok;
}
