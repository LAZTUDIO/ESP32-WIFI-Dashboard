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
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "display.h"
#include "debug_console.h"

#include "app_config.h"
#include "app_actions.h"
#include "app_runtime.h"
#include "dash_metric_store.h"
#include "dashboard_client.h"
#include "device_api.h"
#include "metrics.h"
#include "portal.h"
#include "ui_dynamic.h"
#include "wifi_mgr.h"

#include "cJSON.h"

static const char *TAG = "app";

static app_config_t s_cfg;
static SemaphoreHandle_t s_cfg_lock;
static volatile bool s_server_connected;
static volatile bool s_network_visible;
static volatile int64_t s_server_down_since;   /* 0 表示还没连过服务端 */
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

/* ============================ 运行时配置 ============================ */

void app_runtime_snapshot(app_config_t *out)
{
    if (!out) return;
    if (s_cfg_lock) xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_cfg_lock) xSemaphoreGive(s_cfg_lock);
}

static void ensure_device_id(app_config_t *cfg)
{
    if (!cfg || cfg->device_id[0]) return;
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        snprintf(cfg->device_id, sizeof(cfg->device_id), "esp32-unknown");
    } else {
        snprintf(cfg->device_id, sizeof(cfg->device_id), "esp32-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    if (!config_save(cfg)) {
        ESP_LOGW(TAG, "save device id failed");
    }
    ESP_LOGI(TAG, "device id: %s", cfg->device_id);
}

bool app_runtime_apply_config(const app_config_t *cfg)
{
    if (!cfg) return false;
    app_config_t old;
    app_runtime_snapshot(&old);

    if (s_cfg_lock) xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    bool saved = config_save(cfg);
    if (saved) s_cfg = *cfg;
    if (s_cfg_lock) xSemaphoreGive(s_cfg_lock);
    if (!saved) return false;

    bool need_reload = strcmp(old.dashboard_url, cfg->dashboard_url) != 0 ||
                       strcmp(old.device_id, cfg->device_id) != 0 ||
                       old.rot180 != cfg->rot180;
    if (old.rot180 != cfg->rot180) {
        if (lvgl_port_lock(2000)) {
            display_set_rotation(cfg->rot180 ? 180 : 0);
            lvgl_port_unlock();
        }
    }
    if (need_reload) {
        dashboard_client_reconfigure(cfg->dashboard_url, cfg->device_id,
                                     cfg->rot180);
    }
    if (s_network_visible && !s_server_connected) {
        app_runtime_show_network(true);
    }
    ESP_LOGI(TAG, "runtime config applied (server=%s)",
             cfg->dashboard_url[0] ? cfg->dashboard_url : "(none)");
    return true;
}

bool app_runtime_server_connected(void)
{
    return s_server_connected;
}

void app_runtime_set_server_connected(bool connected)
{
    if (connected) {
        s_server_connected = true;
        s_server_down_since = 0;
        /* 在线期间配置走 WS，管理 HTTP 可以停掉，回收内部 RAM */
        device_api_stop();
        app_runtime_show_network(false);
        return;
    }
    /* WS 失败重连会反复发 DISCONNECTED；只在第一次断开时开始计时，
     * 否则 10 秒宽限永远到不了。 */
    if (s_server_connected || s_server_down_since == 0) {
        s_server_down_since = esp_timer_get_time();
    }
    s_server_connected = false;
}

void app_runtime_show_network(bool show)
{
    if (show && s_server_connected) show = false;
    s_network_visible = show;
    char ip[16] = "";
    wifi_mgr_sta_ip(ip, sizeof(ip));
    app_config_t cfg;
    app_runtime_snapshot(&cfg);
    if (lvgl_port_lock(1000)) {
        ui_dynamic_show_network(show, cfg.wifi_ssid, ip, cfg.device_id,
                                cfg.device_name, s_server_connected);
        lvgl_port_unlock();
    }
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
    device_api_stop();
    app_runtime_show_network(false);
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
    device_api_start();
    time_sync_start();
    portal_overlay_show(false);
    if (!s_server_connected) {
        app_runtime_show_network(true);
    }
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
        /* 服务端掉线 10 秒后重新开放管理 HTTP，允许按 IP 重新接入；
         * 在线时 HTTP 保持关闭，配置由 WS 下发。 */
        if (!s_portal_active && !s_server_connected &&
            wifi_mgr_sta_connected() && !device_api_running()) {
            int64_t now = esp_timer_get_time();
            if (s_server_down_since == 0 ||
                now - s_server_down_since > 10LL * 1000 * 1000) {
                ESP_LOGI(TAG, "server offline, enabling device api");
                device_api_start();
            }
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
        app_config_t cfg;
        app_runtime_snapshot(&cfg);
        if (cfg.server_url[0] && wifi_mgr_sta_connected()) {
            bool ok = metrics_poll(&cfg, &m);
            if (lvgl_port_lock(500)) {
                metrics_publish(&m);
                ui_dynamic_apply();
                lvgl_port_unlock();
            }
            if (!ok) {
                ESP_LOGW(TAG, "api poll failed: %s", cfg.server_url);
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
    s_cfg_lock = xSemaphoreCreateMutex();
    ensure_device_id(&s_cfg);

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

    dashboard_client_start(s_cfg.dashboard_url, s_cfg.device_id, s_cfg.rot180);
    ESP_LOGI(TAG, "dashboard client started%s",
             s_cfg.dashboard_url[0] ? "" : " (waiting for server config)");

    bool connected = false;
    if (s_cfg.wifi_ssid[0]) {
        connected = wifi_mgr_sta_connect(s_cfg.wifi_ssid, s_cfg.wifi_pass, 15000);
        ESP_LOGI(TAG, "wifi connect result: %d", connected);
    }

    if (connected) {
        time_sync_start();
        device_api_start();
        if (!s_server_connected) {
            app_runtime_show_network(true);
        }
    } else {
        ESP_LOGW(TAG, "no Wi-Fi, entering provisioning mode");
        enter_portal();
    }

    xTaskCreate(portal_task, "portal", 4096, NULL, 3, NULL);
    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 3, NULL);
    BaseType_t api_ok = xTaskCreateWithCaps(
        api_task, "api_poll", 8192, NULL, 4, NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (api_ok != pdPASS) {
        ESP_LOGW(TAG, "api task PSRAM stack failed, fallback to internal");
        xTaskCreate(api_task, "api_poll", 8192, NULL, 4, NULL);
    }

    debug_console_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
