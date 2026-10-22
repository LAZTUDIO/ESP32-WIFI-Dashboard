/*
 * ESP32-S3-Touch-LCD-3.5-C —— LLM 服务监控看板
 *
 * 启动顺序：NVS → LVGL → 屏幕/触摸 → 仪表盘 UI
 *           → Wi-Fi（失败则开配网热点） → 指标轮询任务
 *
 * UI 代码在 ../ui，与 PC 无头模拟器共用同一份。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "driver/temperature_sensor.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "display.h"
#include "debug_console.h"

#include "ui.h"
#include "dash_model.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "portal.h"
#include "metrics.h"

static const char *TAG = "app";

static app_config_t s_cfg;
static QueueHandle_t s_actions;
static temperature_sensor_handle_t s_tsens;
static bool s_portal_wait_online;   /* 配网层是在"没网"时打开的，联网后自动退出 */

/* ============================ 动作 ============================ */

static void action_cb(dash_action_t action)
{
    if (s_actions) xQueueSend(s_actions, &action, 0);
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

static void enter_portal(void)
{
    /* 只有在"当前没网"时才允许联网后自动退出；用户主动点重新配网则保留 */
    s_portal_wait_online = !wifi_mgr_sta_connected();
    portal_start(&s_cfg);
    for (int i = 0; i < 10; i++) {
        if (lvgl_port_lock(1000)) {
            dashboard_ui_show_provisioning(true, WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_IP);
            lvgl_port_unlock();
            ESP_LOGI(TAG, "provisioning overlay shown");
            return;
        }
    }
    ESP_LOGE(TAG, "could not lock LVGL to show provisioning overlay");
}

/** 配网期间后台 STA 重连成功：收起浮层、关热点、开始对时 */
static void leave_portal(void)
{
    s_portal_wait_online = false;
    portal_stop();
    time_sync_start();
    if (lvgl_port_lock(500)) {
        dashboard_ui_show_provisioning(false, NULL, NULL, NULL);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "Wi-Fi came up during provisioning, overlay hidden");
}

/* ============================ 采集主循环 ============================ */

static void app_task(void *arg)
{
    (void)arg;
    dash_metrics_t m;

    while (1) {
        dash_action_t act;

        if (xQueueReceive(s_actions, &act, pdMS_TO_TICKS(2000)) == pdTRUE) {
            switch (act) {
            case DASH_ACTION_PROVISION:
                enter_portal();
                break;
            case DASH_ACTION_REFRESH:
                break;      /* 提前唤醒即刷新 */
            case DASH_ACTION_REBOOT:
                ESP_LOGW(TAG, "reboot requested");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
                break;
            case DASH_ACTION_CLEAR_PEAK:
                metrics_reset_peak();
                if (lvgl_port_lock(300)) {
                    dashboard_ui_show_toast("峰值已清空");
                    lvgl_port_unlock();
                }
                break;
            }
        }

        if (s_portal_wait_online && wifi_mgr_sta_connected()) {
            leave_portal();
        }

        metrics_poll(&s_cfg, &m);

        if (s_tsens) {
            float c = 0;
            if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
                m.device_temp_c = (int8_t)c;
            }
        }

        if (lvgl_port_lock(300)) {
            dashboard_ui_apply(&m, true);
            lvgl_port_unlock();
        }
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
    metrics_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_priority = 4;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lv_display_t *disp = display_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    (void)touch_init(disp);

    if (lvgl_port_lock(2000)) {
        dashboard_ui_create();
        dashboard_ui_set_action_cb(action_cb);
        lvgl_port_unlock();
    }

    s_actions = xQueueCreate(8, sizeof(dash_action_t));

    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tsens_cfg, &s_tsens) != ESP_OK ||
        temperature_sensor_enable(s_tsens) != ESP_OK) {
        ESP_LOGW(TAG, "chip temperature sensor unavailable");
        s_tsens = NULL;
    }

    wifi_mgr_init();

    bool connected = false;
    if (s_cfg.wifi_ssid[0]) {
        connected = wifi_mgr_sta_connect(s_cfg.wifi_ssid, s_cfg.wifi_pass, 15000);
    }

    if (connected) {
        time_sync_start();
        if (lvgl_port_lock(500)) {
            dashboard_ui_show_provisioning(false, NULL, NULL, NULL);
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGW(TAG, "no Wi-Fi, entering provisioning mode");
        enter_portal();
    }

    s_actions = xQueueCreate(8, sizeof(dash_action_t));
    xTaskCreate(app_task, "dash_app", 8192, NULL, 5, NULL);

    debug_console_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
