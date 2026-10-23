idf_component_register(
    SRCS "main.c" "display.c" "board_touch.c" "debug_console.c"
         "app_config.c" "wifi_mgr.c" "portal.c" "api_client.c" "metrics.c"
         "dashboard_client.c"
         "../../ui/src/ui_widgets.c" "../../ui/src/ui_dashboard.c"
         "../../ui/src/ui_theme_strix.c" "../../ui/src/ui_theme_tuf.c"
         "../../ui/src/font_cjk_16.c"
         "../../ui/src/dash_metric_store.c" "../../ui/src/ui_dynamic.c"
    INCLUDE_DIRS "." "../../ui/include"
    REQUIRES esp_lcd esp_timer esp_driver_spi esp_driver_gpio esp_driver_i2c
             esp_driver_ledc esp_driver_tsens spi_flash esp_lcd_st7796
             esp_lcd_touch_ft6336 nvs_flash esp_wifi esp_netif
             esp_http_client esp_http_server json spiffs
)

# lvgl / esp_lvgl_port 来自 main/idf_component.yml，组件管理器会自动加入依赖
# lodepng 头文件走 lvgl 组件已导出的 src 目录，无需额外 INCLUDE_DIRS

# 设置页底部显示的固件版本（PC 模拟器不定义，默认显示 "sim"）
target_compile_definitions(${COMPONENT_LIB} PRIVATE DASH_FW_VERSION="v0.1.0")
