#include "display.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#if CONFIG_DEMO_LCD_ENABLE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"
#include "esp_lvgl_port.h"

/* bool 型 Kconfig 未勾选时宏不存在，统一转 0/1 */
#ifdef CONFIG_DEMO_LCD_BGR
#define DEMO_LCD_BGR 1
#else
#define DEMO_LCD_BGR 0
#endif
#ifdef CONFIG_DEMO_LCD_INVERT
#define DEMO_LCD_INVERT 1
#else
#define DEMO_LCD_INVERT 0
#endif
#ifdef CONFIG_DEMO_LCD_SWAP_XY
#define DEMO_LCD_SWAP_XY 1
#else
#define DEMO_LCD_SWAP_XY 0
#endif
#ifdef CONFIG_DEMO_LCD_MIRROR_X
#define DEMO_LCD_MIRROR_X 1
#else
#define DEMO_LCD_MIRROR_X 0
#endif
#ifdef CONFIG_DEMO_LCD_MIRROR_Y
#define DEMO_LCD_MIRROR_Y 1
#else
#define DEMO_LCD_MIRROR_Y 0
#endif
#define LCD_HOST            ((spi_host_device_t)CONFIG_DEMO_LCD_SPI_HOST)
#define BL_LEDC_TIMER       LEDC_TIMER_1
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_FREQ_HZ     5000

/* TCA9554 registers */
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#endif /* CONFIG_DEMO_LCD_ENABLE */

static const char *TAG = "display";

static volatile uint32_t s_frame_cnt;
static lv_display_t *s_lv_disp;

static void on_render_ready(lv_event_t *e)
{
    (void)e;
    s_frame_cnt++;
}

uint32_t display_get_frame_count(void)
{
    return s_frame_cnt;
}

#if CONFIG_DEMO_LCD_ENABLE

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_expander;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

static esp_err_t expander_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_expander, buf, sizeof(buf), 1000);
}

static esp_err_t expander_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_expander, &reg, 1, val, 1, 1000);
}

/*
 * 这块板子的 LCD_RST 没有接到 ESP32 的 GPIO，而是挂在 TCA9554 扩展芯片上。
 * 所以必须在点屏之前通过 I2C 拉一次复位。
 */
static esp_err_t lcd_reset_via_expander(void)
{
    const uint8_t mask = (uint8_t)(1u << CONFIG_DEMO_EXPANDER_RST_PIN);

    ESP_RETURN_ON_ERROR(expander_write(TCA9554_REG_CONFIG, 0x00), TAG, "expander config failed");

    uint8_t out = 0;
    if (expander_read(TCA9554_REG_OUTPUT, &out) != ESP_OK) {
        out = 0;
    }

    const uint8_t asserted = CONFIG_DEMO_EXPANDER_RST_ACTIVE_LOW ? (uint8_t)(out & ~mask) : (uint8_t)(out | mask);
    const uint8_t released = CONFIG_DEMO_EXPANDER_RST_ACTIVE_LOW ? (uint8_t)(out | mask) : (uint8_t)(out & ~mask);

    ESP_RETURN_ON_ERROR(expander_write(TCA9554_REG_OUTPUT, asserted), TAG, "expander reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_REG_OUTPUT, released), TAG, "expander reset release failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t board_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = CONFIG_DEMO_EXPANDER_I2C_SCL,
        .sda_io_num = CONFIG_DEMO_EXPANDER_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c_new_master_bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_DEMO_EXPANDER_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_expander);
}

static esp_err_t backlight_init(void)
{
    if (CONFIG_DEMO_LCD_PIN_BL < 0) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc_timer_config failed");

    ledc_channel_config_t channel = {
        .gpio_num = CONFIG_DEMO_LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc_channel_config failed");

    const uint32_t max_duty = (1u << 10) - 1;
    const uint32_t duty = (uint32_t)CONFIG_DEMO_LCD_BRIGHTNESS * max_duty / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL), TAG, "ledc_update_duty failed");

    ESP_LOGI(TAG, "backlight on GPIO%d at %d%%", CONFIG_DEMO_LCD_PIN_BL, CONFIG_DEMO_LCD_BRIGHTNESS);
    return ESP_OK;
}

static esp_err_t lcd_hw_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_DEMO_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_DEMO_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* 覆盖单次 flush 的最大块（1/12 屏，约 26KB）。
           过大的 max_transfer 会逼 SPI 驱动申请巨大的 DMA 私有缓冲。 */
        .max_transfer_sz = CONFIG_DEMO_LCD_H_RES * 60 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_DEMO_LCD_PIN_CS,
        .dc_gpio_num = CONFIG_DEMO_LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_DEMO_LCD_PIXEL_CLOCK,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_DEMO_LCD_SPI_HOST, &io_cfg, &s_io),
        TAG, "esp_lcd_new_panel_io_spi failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DEMO_LCD_PIN_RST,
        .rgb_ele_order = DEMO_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io, &panel_cfg, &s_panel), TAG, "new_panel_st7796 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, DEMO_LCD_INVERT), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on failed");

    return ESP_OK;
}

static lv_display_t *init_lcd(void)
{
#if CONFIG_DEMO_EXPANDER_ENABLE
    if (board_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed, cannot reset LCD");
        return NULL;
    }
    if (lcd_reset_via_expander() != ESP_OK) {
        ESP_LOGE(TAG, "LCD reset via TCA9554 failed");
        return NULL;
    }
#endif

    if (lcd_hw_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD hardware init failed");
        return NULL;
    }

    if (backlight_init() != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed");
        return NULL;
    }

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        /* 1/12 屏 ×2：内部 DMA 缓冲约 51KB，给 WiFi/HTTP 和 32KB 指令缓存留出余量；
           整屏约 12 次 SPI 传输，实测传输只占 ~5ms */
        .buffer_size = CONFIG_DEMO_LCD_H_RES * CONFIG_DEMO_LCD_V_RES / 12,
        .double_buffer = true,
        .hres = CONFIG_DEMO_LCD_H_RES,
        .vres = CONFIG_DEMO_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = DEMO_LCD_SWAP_XY,
            .mirror_x = DEMO_LCD_MIRROR_X,
            .mirror_y = DEMO_LCD_MIRROR_Y,
        },
        /* 直接让 LVGL 输出字节序已交换的 RGB565，省掉每帧 300KB 的 CPU swap */
        .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .flags = {
            /* 内部 SRAM + DMA：SPI 可直接发送，不再经过 PSRAM→内部 bounce buffer。
               之前缓冲在 PSRAM 时，旋转 270° 会因 DMA 私有缓冲分配失败而永久卡在 flush。 */
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 0,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }

    ESP_LOGI(TAG, "ST7796 %dx%d ready (SPI%d, MOSI=%d SCLK=%d DC=%d)",
             CONFIG_DEMO_LCD_H_RES, CONFIG_DEMO_LCD_V_RES, CONFIG_DEMO_LCD_SPI_HOST,
             CONFIG_DEMO_LCD_PIN_MOSI, CONFIG_DEMO_LCD_PIN_SCLK, CONFIG_DEMO_LCD_PIN_DC);
    return disp;
}

i2c_master_bus_handle_t display_get_i2c_bus(void)
{
    return s_i2c_bus;
}

#else /* !CONFIG_DEMO_LCD_ENABLE */

static void headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static lv_display_t *init_headless(void)
{
    const int32_t hres = CONFIG_DEMO_HEADLESS_H_RES;
    const int32_t vres = CONFIG_DEMO_HEADLESS_V_RES;

    lv_display_t *disp = lv_display_create(hres, vres);
    if (disp == NULL) {
        return NULL;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    const size_t buf_bytes = (size_t)hres * 40 * sizeof(uint16_t);
    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for LVGL draw buffer (%u bytes)", (unsigned)buf_bytes);
        return NULL;
    }
    lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, headless_flush_cb);

    ESP_LOGW(TAG, "headless mode: LVGL runs without a panel (%dx%d, %u B buffer)",
             (int)hres, (int)vres, (unsigned)buf_bytes);
    return disp;
}

#endif /* CONFIG_DEMO_LCD_ENABLE */

lv_display_t *display_init(void)
{
#if CONFIG_DEMO_LCD_ENABLE
    lv_display_t *disp = init_lcd();
#else
    lv_display_t *disp = init_headless();
#endif
    if (disp == NULL) {
        return NULL;
    }

    s_lv_disp = disp;
    lv_display_add_event_cb(disp, on_render_ready, LV_EVENT_RENDER_READY, NULL);
    ESP_LOGI(TAG, "display ready");
    return disp;
}

static void bypass_flush_cb(lv_display_t *disp, const lv_area_t *area,
                            uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static lv_display_flush_cb_t s_orig_flush_cb;

void display_flush_bypass(bool bypass)
{
    if (!s_lv_disp) return;
    if (bypass) {
        if (!s_orig_flush_cb) {
            s_orig_flush_cb = lv_display_get_flush_cb(s_lv_disp);
            lv_display_set_flush_cb(s_lv_disp, bypass_flush_cb);
        }
    } else if (s_orig_flush_cb) {
        lv_display_set_flush_cb(s_lv_disp, s_orig_flush_cb);
        s_orig_flush_cb = NULL;
    }
}

void display_set_rotation(int deg)
{
    if (!s_lv_disp) return;

    lv_display_rotation_t r = LV_DISPLAY_ROTATION_0;
    switch (deg) {
    case 90:  r = LV_DISPLAY_ROTATION_90;  break;
    case 180: r = LV_DISPLAY_ROTATION_180; break;
    case 270: r = LV_DISPLAY_ROTATION_270; break;
    default:  r = LV_DISPLAY_ROTATION_0;   break;
    }
    if (lv_display_get_rotation(s_lv_disp) == r) return;
    lv_display_set_rotation(s_lv_disp, r);
    ESP_LOGI(TAG, "rotation -> %d (%dx%d)", deg,
             (int)lv_display_get_horizontal_resolution(s_lv_disp),
             (int)lv_display_get_vertical_resolution(s_lv_disp));
}
