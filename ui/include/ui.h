/**
 * UI 设计系统 —— 320×480 竖屏 ESP32-WIFI-Dashboard
 *
 * 同一份 UI 代码同时编译到 PC 模拟器与 ESP32-S3 固件。
 * 约束：ST7796 SPI 屏，深色主题，靠描边和字号层级建立层次，
 *      不用重阴影/大面积半透明，保证滚动与刷新流畅。
 *
 * 配色不再写死在固件里：服务端把 theme_tokens 注入布局 JSON，
 * ui_dynamic 解析后通过 ui_theme_set() 应用到所有控件。
 * 下面的宏只是"经典"配色的兜底默认值。
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(lv_font_cjk_16);

/* ============================== 设计 Token（默认值） ============================== */

#define UI_BG        0x070B14u
#define UI_SURFACE   0x0B1424u
#define UI_SURFACE_2 0x101B30u
#define UI_BORDER    0x1B4A5Au

#define UI_TEXT      0xC9E8F2u
#define UI_TEXT_DIM  0x5F8296u
#define UI_TEXT_MUT  0x3E5A6Bu

#define UI_ACCENT    0x00E5FFu
#define UI_ACCENT_2  0xFF3D8Bu
#define UI_CYAN      0x00E5FFu
#define UI_OK        0x9DFF00u
#define UI_WARN      0xFFB300u
#define UI_ERR       0xFF5347u
#define UI_PINK      0xFF78A0u

#define UI_SP_XS 4
#define UI_SP_S  8
#define UI_SP_M  12
#define UI_SP_L  16
#define UI_SP_XL 20

#define UI_R_S    8
#define UI_R_M    12
#define UI_R_L    16
#define UI_R_XL   20
#define UI_R_PILL 999

#define UI_F_DISPLAY &lv_font_montserrat_48
#define UI_F_H1       &lv_font_montserrat_32
#define UI_F_H2       &lv_font_montserrat_28
#define UI_F_H3       &lv_font_montserrat_20
#define UI_F_BODY     &lv_font_montserrat_16
#define UI_F_CAP      &lv_font_montserrat_14
#define UI_F_CJK      &lv_font_cjk_16

#define UI_SCREEN_W 320
#define UI_SCREEN_H 480

/* ============================== 运行时配色 ============================== */

typedef enum {
    UI_SKIN_NEON = 0,
    UI_SKIN_MINIMAL,
    UI_SKIN_VINTAGE,
    UI_SKIN_VITAL,
} ui_skin_t;

typedef enum {
    UI_FONT_SANS = 0,
    UI_FONT_MONO,
} ui_font_style_t;

typedef enum {
    UI_DECO_NONE = 0,
    UI_DECO_CORNERS,
    UI_DECO_TICKS,
    UI_DECO_PULSE,
} ui_deco_t;

typedef enum {
    UI_PET_CYBER = 0,
    UI_PET_BLOB,
    UI_PET_BRASS,
    UI_PET_PULSE,
} ui_pet_style_t;

typedef struct {
    uint32_t bg;
    uint32_t surface;
    uint32_t surface_2;
    uint32_t border;
    uint32_t text;
    uint32_t text_dim;
    uint32_t text_mut;
    uint32_t accent;
    uint32_t accent_2;
    uint32_t cyan;
    uint32_t ok;
    uint32_t warn;
    uint32_t err;
    uint32_t pink;
    int radius;
    /* 四套皮肤扩展字段 */
    ui_skin_t skin;
    uint32_t surface_grad_from;
    uint32_t surface_grad_to;
    uint32_t glow;
    int glow_opa;
    int border_width;
    int corner_len;
    ui_font_style_t font_style;
    int tracking;
    ui_deco_t deco;
    ui_pet_style_t pet_style;
} ui_theme_t;

/** 把 t 填成经典配色（不依赖当前主题）。 */
void ui_theme_default(ui_theme_t *t);

/** 当前生效的配色；创建控件时读取。 */
const ui_theme_t *ui_theme(void);

/** 替换当前配色；之后的控件与重绘生效。 */
void ui_theme_set(const ui_theme_t *t);

/* ============================== 通用组件 ============================== */

lv_obj_t * ui_group_create(lv_obj_t * parent);
lv_obj_t * ui_card_create(lv_obj_t * parent);
lv_obj_t * ui_pill_create(lv_obj_t * parent, const char * text, uint32_t color);

/* 双序列折线图；第二序列传 NULL 时只启用一条。 */
lv_obj_t * ui_chart_create(lv_obj_t * parent, int32_t height,
                           uint32_t color1, uint32_t color2,
                           lv_chart_series_t ** s1, lv_chart_series_t ** s2);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
