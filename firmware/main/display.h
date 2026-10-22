#pragma once

#include "sdkconfig.h"
#include "lvgl.h"
#include "esp_err.h"

#if CONFIG_DEMO_LCD_ENABLE && CONFIG_DEMO_EXPANDER_ENABLE
#include "driver/i2c_master.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化显示并把它注册给 LVGL
 *
 * - 打开 CONFIG_DEMO_LCD_ENABLE：初始化 ST7796（含 TCA9554 复位、LEDC 背光）
 * - 关闭时：创建一块虚拟屏幕，LVGL 正常渲染但画面丢弃（只用于验证工具链）
 *
 * @return LVGL display 句柄，失败返回 NULL
 */
lv_display_t *display_init(void);

/**
 * @brief 已经完成渲染的帧数（由 LV_EVENT_RENDER_READY 统计）
 */
uint32_t display_get_frame_count(void);

/**
 * @brief 运行时旋转屏幕（0/90/180/270）
 *
 * 由 esp_lvgl_port 走硬件 MADCTL（swap_xy + mirror），不占 CPU。
 * 必须在 LVGL 锁内调用；触摸坐标由 LVGL 自动换算。
 */
void display_set_rotation(int deg);

/** 诊断用：临时跳过 SPI 发送，用于分离「渲染耗时」和「传输耗时」 */
void display_flush_bypass(bool bypass);

#if CONFIG_DEMO_LCD_ENABLE && CONFIG_DEMO_EXPANDER_ENABLE

/**
 * @brief 板载 I2C 总线（TCA9554 与 FT6336 共用）
 */
i2c_master_bus_handle_t display_get_i2c_bus(void);

/**
 * @brief 初始化 FT6336 电容触摸并注册成 LVGL 输入设备
 */
esp_err_t touch_init(lv_display_t *disp);

#endif

#ifdef __cplusplus
}
#endif
