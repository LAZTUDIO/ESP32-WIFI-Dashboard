#include "sdkconfig.h"

#if CONFIG_DEMO_LCD_ENABLE && CONFIG_DEMO_EXPANDER_ENABLE

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft6336.h"
#include "esp_lvgl_port.h"

#include "display.h"

static const char *TAG = "touch";

esp_err_t touch_init(lv_display_t *disp)
{
    i2c_master_bus_handle_t bus = display_get_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT6336_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags.disable_control_phase = 1,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &tp_io), TAG, "new_panel_io_i2c failed");

    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = CONFIG_DEMO_LCD_H_RES,
        .y_max = CONFIG_DEMO_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft6336(tp_io, &tp_cfg, &tp), TAG, "ft6336 init failed");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };
    if (lvgl_port_add_touch(&touch_cfg) == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FT6336 touch ready (%dx%d)", CONFIG_DEMO_LCD_H_RES, CONFIG_DEMO_LCD_V_RES);
    return ESP_OK;
}

#endif /* CONFIG_DEMO_LCD_ENABLE && CONFIG_DEMO_EXPANDER_ENABLE */
