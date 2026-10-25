#pragma once

#include "lvgl.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UI_PET_SLEEP = 0,
    UI_PET_IDLE,
    UI_PET_HAPPY,
    UI_PET_BUSY,
    UI_PET_ALERT,
    UI_PET_OFFLINE,
};

/**
 * LVGL 矢量桌宠。结构体由调用方持有，不额外分配内存；
 * 所有对象都是 parent 的子对象，随界面一起销毁。
 */
typedef struct {
    lv_obj_t *shadow;
    lv_obj_t *aura;
    lv_obj_t *actor;
    lv_obj_t *torso;
    lv_obj_t *head;
    lv_obj_t *ear_l;
    lv_obj_t *ear_r;
    lv_obj_t *tail;
    lv_obj_t *eye_l;
    lv_obj_t *eye_r;
    lv_obj_t *eye_closed_l;
    lv_obj_t *eye_closed_r;
    lv_obj_t *cheek_l;
    lv_obj_t *cheek_r;
    lv_obj_t *mouth;
    lv_obj_t *fx;

    ui_pet_style_t style;
    int state;
    int size;
    int base_x;
    int base_y;
    uint32_t t0;
    bool closed_visible;
} ui_pet_t;

void ui_pet_create(ui_pet_t *pet, lv_obj_t *parent, int parent_w, int parent_h,
                   ui_pet_style_t style);
void ui_pet_set_state(ui_pet_t *pet, int state);
void ui_pet_tick(ui_pet_t *pet, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
