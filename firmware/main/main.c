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
#include <stdlib.h>

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
#include "dashboard_client.h"
#include "ui_dynamic.h"

#include "cJSON.h"

static const char *TAG = "app";

static app_config_t s_cfg;
static QueueHandle_t s_actions;

/* 0.2 动态看板的 JSON 全部走 PSRAM，给内部 RAM 留出余量 */
static void *json_psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}
static temperature_sensor_handle_t s_tsens;
static bool s_portal_wait_online;   /* 配网层是在"没网"时打开的，联网后自动退出 */

/* ============================ 动作 ============================ */

static void action_cb(dash_action_t action)
{
    if (s_actions) xQueueSend(s_actions, &action, 0);
}

/** 主题默认方向 + 用户 180° 翻转 */
static int theme_rotation(void)
{
    int deg = (s_cfg.theme == DASH_THEME_STRIX) ? 90 : 0;
    if (s_cfg.rot180) deg = (deg + 180) % 360;
    return deg;
}

/** 应用主题与方向；调用方必须已持有 LVGL 锁 */
static void apply_view(bool rebuild)
{
    display_set_rotation(theme_rotation());
    dashboard_ui_set_prefs((dash_theme_t)s_cfg.theme, s_cfg.rot180);
    if (rebuild) dashboard_ui_set_theme((dash_theme_t)s_cfg.theme);
}

/** 串口 t / r 命令：直接切主题或翻转，便于无触摸时验证 */
static void dbg_view_cb(int theme, int rot180)
{
    if (theme >= 0 && theme < DASH_THEME_COUNT) s_cfg.theme = (uint8_t)theme;
    if (rot180 == 2) s_cfg.rot180 = !s_cfg.rot180;
    else if (rot180 >= 0) s_cfg.rot180 = (rot180 != 0);

    config_save(&s_cfg);
    if (lvgl_port_lock(1500)) {
        apply_view(true);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "view theme=%u rot180=%d rot=%d", s_cfg.theme,
             s_cfg.rot180, theme_rotation());
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

            case DASH_ACTION_THEME_CLASSIC:
            case DASH_ACTION_THEME_STRIX:
            case DASH_ACTION_THEME_TUF: {
                uint8_t t = (act == DASH_ACTION_THEME_STRIX) ? 1 :
                            (act == DASH_ACTION_THEME_TUF) ? 2 : 0;
                if (t != s_cfg.theme) {
                    s_cfg.theme = t;
                    config_save(&s_cfg);
                    if (lvgl_port_lock(800)) {
                        apply_view(true);
                        lvgl_port_unlock();
                    }
                }
                break;
            }

            case DASH_ACTION_TOGGLE_ROT180:
                s_cfg.rot180 = !s_cfg.rot180;
                config_save(&s_cfg);
                if (lvgl_port_lock(800)) {
                    apply_view(false);
                    lvgl_port_unlock();
                }
                break;
            }
        }

        if (s_portal_wait_online && wifi_mgr_sta_connected()) {
            leave_portal();
        }

        /* 动态看板接管屏幕后，停掉旧版轮询和绘制 */
        if (ui_dynamic_active()) continue;

        metrics_poll(&s_cfg, &m);

        if (s_tsens) {
            float c = 0;
            if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
                m.device_temp_c = (int8_t)c;
            }
        }

        if (lvgl_port_lock(300)) {
            int64_t t0 = esp_timer_get_time();
            dashboard_ui_apply(&m, true);
            int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
            lvgl_port_unlock();
            if (dt_ms > 30) ESP_LOGI(TAG, "ui apply %lld ms", (long long)dt_ms);
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
    if (s_cfg.theme >= DASH_THEME_COUNT) s_cfg.theme = DASH_THEME_CLASSIC;

    cJSON_Hooks json_hooks = {
        .malloc_fn = json_psram_malloc,
        .free_fn = heap_caps_free,
    };
    cJSON_InitHooks(&json_hooks);

    s_actions = xQueueCreate(8, sizeof(dash_action_t));
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
        display_set_rotation(theme_rotation());
        dashboard_ui_set_prefs((dash_theme_t)s_cfg.theme, s_cfg.rot180);
        dashboard_ui_create();
        dashboard_ui_set_action_cb(action_cb);
        lvgl_port_unlock();
    }

    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tsens_cfg, &s_tsens) != ESP_OK ||
        temperature_sensor_enable(s_tsens) != ESP_OK) {
        ESP_LOGW(TAG, "chip temperature sensor unavailable");
        s_tsens = NULL;
    }

    wifi_mgr_init();

    if (s_cfg.dashboard_url[0]) {
        dashboard_client_start(s_cfg.dashboard_url, s_cfg.rot180);
    }

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

    xTaskCreate(app_task, "dash_app", 8192, NULL, 5, NULL);

    debug_console_set_view_cb(dbg_view_cb);
    debug_console_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
