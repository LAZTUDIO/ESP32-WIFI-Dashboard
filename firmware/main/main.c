/*
 * ESP32-S3-Touch-LCD-3.5-C —— ESP32-WIFI-Dashboard
 *
 * 启动顺序：NVS → LVGL → 屏幕/触摸 → Wi-Fi（失败则开配网热点）
 *           → 动态看板客户端（拉布局 / WS 指标）
 *
 * UI 代码在 ../ui，动态布局与配色全部由服务端下发。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "display.h"
#include "debug_console.h"

#include "app_config.h"
#include "app_actions.h"
#include "dash_metric_store.h"
#include "dashboard_client.h"
#include "metrics.h"
#include "portal.h"
#include "ui_dynamic.h"
#include "wifi_mgr.h"

#include "cJSON.h"

static const char *TAG = "app";

static app_config_t s_cfg;
static bool s_portal_wait_online;   /* 配网层是在"没网"时打开的，联网后自动退出 */
static bool s_portal_active;

/* 0.2 动态看板的 JSON 全部走 PSRAM，给内部 RAM 留出余量 */
static void *json_psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

/* ============================ 时间同步 ============================ */

static void time_sync_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_init();
}

/* ============================ 配网 ============================ */

static void portal_overlay_show(bool show)
{
    if (!lvgl_port_lock(1000)) return;
    ui_dynamic_show_portal(show, WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_IP);
    lvgl_port_unlock();
}

static void enter_portal(void)
{
    if (s_portal_active) {
        ESP_LOGI(TAG, "provisioning already active");
        return;
    }
    s_portal_active = true;
    /* 只有在"当前没网"时才允许联网后自动退出；用户主动进配网则保留 */
    s_portal_wait_online = !wifi_mgr_sta_connected();
    portal_start(&s_cfg);
    portal_overlay_show(true);
    ESP_LOGI(TAG, "provisioning mode: AP %s", WIFI_AP_SSID);
}

/** 配网期间后台 STA 重连成功：收起浮层、关热点、开始对时 */
static void leave_portal(void)
{
    s_portal_active = false;
    s_portal_wait_online = false;
    portal_stop();
    time_sync_start();
    portal_overlay_show(false);
    ESP_LOGI(TAG, "Wi-Fi came up during provisioning, overlay hidden");
}

void app_request_provisioning(void)
{
    enter_portal();
}

/* ============================ 后台任务 ============================ */

static void portal_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_portal_wait_online && wifi_mgr_sta_connected()) {
            leave_portal();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/** 传统 API 直连：轮询结果写进与 WS 共用的指标存储，再刷新动态 UI。 */
static void api_task(void *arg)
{
    (void)arg;
    dash_metrics_t m;
    while (1) {
        if (s_cfg.server_url[0] && wifi_mgr_sta_connected()) {
            bool ok = metrics_poll(&s_cfg, &m);
            if (lvgl_port_lock(500)) {
                metrics_publish(&m);
                ui_dynamic_apply();
                lvgl_port_unlock();
            }
            if (!ok) {
                ESP_LOGW(TAG, "api poll failed: %s", s_cfg.server_url);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* BOOT (GPIO0) 低电平长按 3 秒进入配网 */
static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    bool fired = false;
    while (1) {
        if (gpio_get_level(0) == 0) {
            held_ms += 50;
            if (!fired && held_ms >= 3000) {
                fired = true;
                ESP_LOGW(TAG, "BOOT long press -> provisioning");
                app_request_provisioning();
            }
        } else {
            held_ms = 0;
            fired = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ============================ 入口 ============================ */

void app_main(void)
{
    ESP_LOGI(TAG, "LLM dashboard booting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    config_load(&s_cfg);

    cJSON_Hooks json_hooks = {
        .malloc_fn = json_psram_malloc,
        .free_fn = heap_caps_free,
    };
    cJSON_InitHooks(&json_hooks);

    if (!dash_metric_store_init()) {
        ESP_LOGE(TAG, "metric store init failed");
    }
    metrics_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 12288;
    port_cfg.task_priority = 4;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lv_display_t *disp = display_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    (void)touch_init(disp);

    if (lvgl_port_lock(2000)) {
        display_set_rotation(s_cfg.rot180 ? 180 : 0);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "rotation applied");

    wifi_mgr_init();
    ESP_LOGI(TAG, "wifi manager ready");

    if (s_cfg.dashboard_url[0]) {
        dashboard_client_start(s_cfg.dashboard_url, s_cfg.rot180);
        ESP_LOGI(TAG, "dashboard client started");
    }

    bool connected = false;
    if (s_cfg.wifi_ssid[0]) {
        connected = wifi_mgr_sta_connect(s_cfg.wifi_ssid, s_cfg.wifi_pass, 15000);
        ESP_LOGI(TAG, "wifi connect result: %d", connected);
    }

    if (connected) {
        time_sync_start();
        if (!s_cfg.dashboard_url[0]) {
            ESP_LOGW(TAG, "no dashboard URL, entering provisioning mode");
            enter_portal();
        }
    } else {
        ESP_LOGW(TAG, "no Wi-Fi, entering provisioning mode");
        enter_portal();
    }

    xTaskCreate(portal_task, "portal", 3072, NULL, 3, NULL);
    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 3, NULL);
    if (s_cfg.server_url[0]) {
        xTaskCreate(api_task, "api_poll", 8192, NULL, 4, NULL);
    }

    debug_console_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
