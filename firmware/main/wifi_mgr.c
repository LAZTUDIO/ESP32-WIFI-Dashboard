#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "wifi_mgr.h"

static const char *TAG = "wifi";

#define BIT_OK   BIT0
#define BIT_FAIL BIT1

static EventGroupHandle_t s_eg;
static bool s_inited;
static bool s_ap_on;
static bool s_auto_reconnect;   /* 掉线后是否自动重连（配网模式下也保持） */
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_eg, BIT_OK);
            xEventGroupSetBits(s_eg, BIT_FAIL);
            ESP_LOGW(TAG, "STA disconnected");
            /* 路由器上电慢、首次 DHCP 慢都会导致一次失败，不能就此躺平。
               断开事件本身要等一次完整失败（数秒）才产生，不会形成忙循环 */
            if (s_auto_reconnect) esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "STA started");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupClearBits(s_eg, BIT_FAIL);
        xEventGroupSetBits(s_eg, BIT_OK);
    }
}

esp_err_t wifi_mgr_init(void)
{
    if (s_inited) return ESP_OK;

    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_inited = true;
    return ESP_OK;
}

bool wifi_mgr_sta_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_inited || !ssid || !ssid[0]) return false;

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass && pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_eg, BIT_OK | BIT_FAIL);

    if (wifi_mgr_ap_running()) esp_wifi_set_mode(WIFI_MODE_APSTA);
    else esp_wifi_set_mode(WIFI_MODE_STA);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_auto_reconnect = true;
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_OK | BIT_FAIL, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_OK) {
        ESP_LOGI(TAG, "connected to %s", ssid);
        return true;
    }

    /* 首次连接超时不放弃：保持自动重连，交给上层决定是否开配网热点 */
    ESP_LOGW(TAG, "connect to %s not ready in %d ms, keep retrying",
             ssid, timeout_ms);
    return false;
}

bool wifi_mgr_auto_reconnect(bool on)
{
    bool prev = s_auto_reconnect;
    s_auto_reconnect = on;
    return prev;
}

void wifi_mgr_sta_disconnect(void)
{
    s_auto_reconnect = false;
    if (s_inited) esp_wifi_disconnect();
}

bool wifi_mgr_sta_connected(void)
{
    if (!s_inited) return false;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool wifi_mgr_sta_ip(char *out, size_t n)
{
    if (!s_sta_netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

int8_t wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_inited && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

bool wifi_mgr_ap_start(void)
{
    if (!s_inited) return false;
    if (s_ap_on) return true;

    wifi_config_t apc = {0};
    strncpy((char *)apc.ap.ssid, WIFI_AP_SSID, sizeof(apc.ap.ssid) - 1);
    apc.ap.ssid_len = strlen(WIFI_AP_SSID);
    strncpy((char *)apc.ap.password, WIFI_AP_PASS, sizeof(apc.ap.password) - 1);
    apc.ap.max_connection = 4;
    apc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    apc.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));

    s_ap_on = true;
    ESP_LOGI(TAG, "SoftAP up: %s / %s", WIFI_AP_SSID, WIFI_AP_PASS);
    return true;
}

void wifi_mgr_ap_stop(void)
{
    if (!s_inited || !s_ap_on) return;
    esp_wifi_set_mode(wifi_mgr_sta_connected() ? WIFI_MODE_STA : WIFI_MODE_APSTA);
    s_ap_on = false;
}

bool wifi_mgr_ap_running(void)
{
    return s_ap_on;
}

int wifi_mgr_scan(char *out, size_t cap, int max_items)
{
    if (!s_inited) return 0;

    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;

    wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&num, recs);

    size_t used = 0;
    int count = 0;
    out[0] = '\0';
    for (int i = 0; i < num && count < max_items; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        int n = snprintf(out + used, cap - used, "%s|%d\n",
                         (const char *)recs[i].ssid, recs[i].rssi);
        if (n <= 0 || (size_t)n >= cap - used) break;
        used += n;
        count++;
    }
    free(recs);
    return count;
}
