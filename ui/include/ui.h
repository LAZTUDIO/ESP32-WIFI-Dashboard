/**
 * UI 设计系统 —— 320×480 竖屏 LLM 服务监控看板
 *
 * 同一份 UI 代码同时编译到 PC 无头模拟器与 ESP32-S3 固件。
 * 约束：ST7796 SPI 屏，深色主题，靠描边和字号层级建立层次，
 *      不用重阴影/大面积半透明，保证滚动与刷新流畅。
 */
#ifndef UI_H
#define UI_H

#include "lvgl.h"
#include "dash_model.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(lv_font_cjk_16);

/* ============================== 设计 Token ============================== */

#define UI_BG        0x080B10u
#define UI_SURFACE   0x121822u
#define UI_SURFACE_2 0x18202Cu
#define UI_BORDER    0x263242u

#define UI_TEXT      0xEDF2F8u
#define UI_TEXT_DIM  0x8C9BAEu
#define UI_TEXT_MUT  0x5A6B80u

#define UI_ACCENT    0x4D8DFFu
#define UI_ACCENT_2  0x8B5CF6u
#define UI_CYAN      0x22D3EEu
#define UI_OK        0x34D399u
#define UI_WARN      0xFB923Cu
#define UI_ERR       0xF87171u
#define UI_PINK      0xF472B6u

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

/* ============================== 通用组件 ============================== */

lv_obj_t * ui_card_create(lv_obj_t * parent);
lv_obj_t * ui_group_create(lv_obj_t * parent);
lv_obj_t * ui_label_create(lv_obj_t * parent, const char * text);
lv_obj_t * ui_cjk_create(lv_obj_t * parent, const char * text);
lv_obj_t * ui_caption_create(lv_obj_t * parent, const char * text);
lv_obj_t * ui_pill_create(lv_obj_t * parent, const char * text, uint32_t color);
lv_obj_t * ui_bar_create(lv_obj_t * parent, int32_t max, uint32_t color);

/* 双序列折线图；第二序列传 NULL 时只启用一条。 */
lv_obj_t * ui_chart_create(lv_obj_t * parent, int32_t height,
                           uint32_t color1, uint32_t color2,
                           lv_chart_series_t ** s1, lv_chart_series_t ** s2);

/* ============================== 主题 ============================== */

/** 三套主题：经典竖屏 / ROG 风横屏 / 军工琥珀竖屏 */
typedef enum {
    DASH_THEME_CLASSIC = 0,     /* 320×480 石墨蓝 */
    DASH_THEME_STRIX  = 1,      /* 480×320 红黑斜切 */
    DASH_THEME_TUF    = 2,      /* 320×480 石墨琥珀 */
    DASH_THEME_COUNT
} dash_theme_t;

#define UI_STRIX_W 480
#define UI_STRIX_H 320

/* ============================== 页面 ============================== */

typedef enum {
    DASH_ACTION_PROVISION = 0,
    DASH_ACTION_REFRESH,
    DASH_ACTION_REBOOT,
    DASH_ACTION_CLEAR_PEAK,
    DASH_ACTION_THEME_CLASSIC,
    DASH_ACTION_THEME_STRIX,
    DASH_ACTION_THEME_TUF,
    DASH_ACTION_TOGGLE_ROT180,
} dash_action_t;

typedef void (* dash_action_cb_t)(dash_action_t action);

/* 必须在 dashboard_ui_create() 之前调用，决定首次创建哪套主题 */
void dashboard_ui_set_prefs(dash_theme_t theme, bool rot180);
dash_theme_t dashboard_ui_theme(void);
bool dashboard_ui_rot180(void);

void dashboard_ui_create(void);
void dashboard_ui_destroy(void);
void dashboard_ui_apply(const dash_metrics_t * m, bool new_sample);
void dashboard_ui_set_action_cb(dash_action_cb_t cb);
void dashboard_ui_set_theme(dash_theme_t theme);
void dashboard_ui_set_page(int page);
int  dashboard_ui_page(void);
int  dashboard_ui_page_count(void);
void dashboard_ui_show_provisioning(bool show, const char * ssid,
                                    const char * password, const char * ip);
void dashboard_ui_show_toast(const char * text);

/* 把 API key 脱敏成 "sk-lm-…abcd" 形式，写入 out。 */
void ui_mask_key(const char * key, char * out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
