#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"

#include "api_client.h"

static const char *TAG = "http";

int api_get(const char *url, const char *bearer, char *out, size_t cap,
            int timeout_ms)
{
    if (!url || !out || cap < 2) return -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = false,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        ESP_LOGE(TAG, "client init failed: %s", url);
        return -1;
    }

    esp_http_client_set_header(h, "Accept", "application/json");
    esp_http_client_set_header(h, "Accept-Encoding", "identity");

    char auth[224];
    if (bearer && bearer[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(h, "Authorization", auth);
    }

    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed (%s): %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(h);
        return -1;
    }

    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return -2;
    }

    size_t used = 0;
    while (used + 1 < cap) {
        int r = esp_http_client_read(h, out + used, (int)(cap - 1 - used));
        if (r < 0) {
            ESP_LOGW(TAG, "read failed: %s", url);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return -1;
        }
        if (r == 0) break;
        used += (size_t)r;
    }
    out[used] = '\0';

    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return (int)used;
}

int api_get_path(const char *base, const char *path, const char *bearer,
                 char *out, size_t cap, int timeout_ms)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);
    return api_get(url, bearer, out, cap, timeout_ms);
}
