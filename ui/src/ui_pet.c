#include "ui_pet.h"

#include <math.h>
#include <string.h>

#include "ui.h"

static void show(lv_obj_t *o, bool visible)
{
    if (!o) return;
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *part(lv_obj_t *parent, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void place(lv_obj_t *o, int x, int y, int w, int h)
{
    if (!o) return;
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
}

static uint32_t lerp_color(uint32_t from, uint32_t to, int t)
{
    if (t <= 0) return from;
    if (t >= 100) return to;
    int fr = (from >> 16) & 0xFF, fg = (from >> 8) & 0xFF, fb = from & 0xFF;
    int tr = (to >> 16) & 0xFF, tg = (to >> 8) & 0xFF, tb = to & 0xFF;
    int r = (fr * (100 - t) + tr * t) / 100;
    int g = (fg * (100 - t) + tg * t) / 100;
    int b = (fb * (100 - t) + tb * t) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 状态只做轻度着色：头部明显，躯干保持深色，避免整体糊成一块。 */
static void state_tint(int state, const ui_theme_t *th, uint32_t *tint,
                       int *head_mix, int *torso_mix)
{
    *tint = th->accent_2;
    *head_mix = 0;
    *torso_mix = 0;
    switch (state) {
    case UI_PET_SLEEP:
        *head_mix = 45;
        *torso_mix = 12;
        break;
    case UI_PET_HAPPY:
        *head_mix = 8;
        break;
    case UI_PET_ALERT:
        *tint = th->err;
        *head_mix = 55;
        *torso_mix = 18;
        break;
    case UI_PET_OFFLINE:
        *tint = th->text_mut;
        *head_mix = 75;
        *torso_mix = 55;
        break;
    default:
        break;
    }
}

void ui_pet_create(ui_pet_t *pet, lv_obj_t *parent, int parent_w, int parent_h,
                   ui_pet_style_t style)
{
    if (!pet || !parent) return;
    memset(pet, 0, sizeof(*pet));
    pet->style = style;
    pet->state = -1;
    pet->t0 = lv_tick_get();
    if (pet->t0 == 0) pet->t0 = 1;

    const ui_theme_t *th = ui_theme();
    int s = (parent_w < parent_h ? parent_w : parent_h) * 86 / 100;
    if (s > 120) s = 120;
    if (s < 32) s = 32;
    pet->size = s;
    pet->base_x = (parent_w - s) / 2;
    pet->base_y = (parent_h - s) / 2;
    if (pet->base_y < 0) pet->base_y = 0;

    uint32_t head = th->accent;
    uint32_t torso = th->surface_2;
    uint32_t second = th->accent_2;
    uint32_t eye = th->bg;
    uint32_t cheek = th->pink;
    int head_radius = LV_RADIUS_CIRCLE;
    int torso_radius = 8;

    if (style == UI_PET_CYBER) {
        head_radius = 5;
        torso_radius = 6;
    } else if (style == UI_PET_BLOB) {
        torso_radius = LV_RADIUS_CIRCLE;
        torso = th->accent;
    } else if (style == UI_PET_BRASS) {
        head = th->accent_2;
        torso = th->surface_2;
        second = th->accent;
    } else if (style == UI_PET_PULSE) {
        head_radius = 10;
        torso_radius = 14;
        torso = th->surface_2;
        second = th->accent_2;
    }

    pet->shadow = part(parent, th->text_mut, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(pet->shadow, LV_OPA_30, 0);
    place(pet->shadow, pet->base_x + s * 23 / 100, pet->base_y + s * 91 / 100,
          s * 54 / 100, s * 7 / 100);

    if (style == UI_PET_PULSE) {
        pet->aura = part(parent, th->accent, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(pet->aura, LV_OPA_20, 0);
        place(pet->aura, pet->base_x, pet->base_y, s, s);
    }

    pet->actor = lv_obj_create(parent);
    lv_obj_remove_style_all(pet->actor);
    lv_obj_remove_flag(pet->actor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pet->actor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(pet->actor, pet->base_x, pet->base_y);
    lv_obj_set_size(pet->actor, s, s);

    pet->torso = part(pet->actor, torso, torso_radius);
    place(pet->torso, s * 22 / 100, s * 57 / 100, s * 56 / 100, s * 30 / 100);
    if (style == UI_PET_CYBER || style == UI_PET_BRASS) {
        lv_obj_set_style_border_width(pet->torso, 1, 0);
        lv_obj_set_style_border_color(pet->torso, lv_color_hex(second), 0);
        lv_obj_set_style_border_opa(pet->torso, LV_OPA_50, 0);
    }

    pet->head = part(pet->actor, head, head_radius);
    place(pet->head, s * 20 / 100, s * 10 / 100, s * 60 / 100, s * 50 / 100);
    if (style == UI_PET_BRASS || style == UI_PET_CYBER) {
        lv_obj_set_style_border_width(pet->head, 1, 0);
        lv_obj_set_style_border_color(pet->head, lv_color_hex(second), 0);
        lv_obj_set_style_border_opa(pet->head, LV_OPA_60, 0);
    }

    if (style != UI_PET_BLOB && style != UI_PET_PULSE) {
        pet->ear_l = part(pet->actor, second, style == UI_PET_CYBER ? 3 : LV_RADIUS_CIRCLE);
        pet->ear_r = part(pet->actor, second, style == UI_PET_CYBER ? 3 : LV_RADIUS_CIRCLE);
        place(pet->ear_l, s * 19 / 100, s * 2 / 100, s * 15 / 100, s * 16 / 100);
        place(pet->ear_r, s * 66 / 100, s * 2 / 100, s * 15 / 100, s * 16 / 100);
    }

    if (style == UI_PET_CYBER || style == UI_PET_BRASS) {
        pet->tail = part(pet->actor, second, style == UI_PET_CYBER ? 2 : 3);
        place(pet->tail, s * 80 / 100, s * 62 / 100, s * 11 / 100, s * 15 / 100);
    }

    pet->eye_l = part(pet->actor, eye, style == UI_PET_CYBER ? 2 : LV_RADIUS_CIRCLE);
    pet->eye_r = part(pet->actor, eye, style == UI_PET_CYBER ? 2 : LV_RADIUS_CIRCLE);
    place(pet->eye_l, s * 34 / 100, s * 31 / 100, s * 8 / 100, s * 11 / 100);
    place(pet->eye_r, s * 58 / 100, s * 31 / 100, s * 8 / 100, s * 11 / 100);

    pet->eye_closed_l = part(pet->actor, eye, 1);
    pet->eye_closed_r = part(pet->actor, eye, 1);
    place(pet->eye_closed_l, s * 33 / 100, s * 35 / 100, s * 9 / 100, 2);
    place(pet->eye_closed_r, s * 58 / 100, s * 35 / 100, s * 9 / 100, 2);
    show(pet->eye_closed_l, false);
    show(pet->eye_closed_r, false);

    pet->cheek_l = part(pet->actor, cheek, LV_RADIUS_CIRCLE);
    pet->cheek_r = part(pet->actor, cheek, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(pet->cheek_l, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(pet->cheek_r, LV_OPA_60, 0);
    place(pet->cheek_l, s * 24 / 100, s * 45 / 100, s * 10 / 100, s * 5 / 100);
    place(pet->cheek_r, s * 66 / 100, s * 45 / 100, s * 10 / 100, s * 5 / 100);

    pet->mouth = part(pet->actor, eye, 2);
    place(pet->mouth, s * 43 / 100, s * 52 / 100, s * 14 / 100, s * 3 / 100);

    pet->fx = lv_label_create(pet->actor);
    lv_label_set_text(pet->fx, "");
    lv_obj_set_style_text_font(pet->fx, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(pet->fx, lv_color_hex(th->text_dim), 0);
    lv_obj_set_pos(pet->fx, s * 68 / 100, 0);
    lv_obj_set_size(pet->fx, s * 32 / 100, 16);
    lv_obj_set_style_text_align(pet->fx, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(pet->fx, LV_OPA_TRANSP, 0);

    ui_pet_set_state(pet, UI_PET_IDLE);
}

static void set_palette(ui_pet_t *pet, int state)
{
    const ui_theme_t *th = ui_theme();
    uint32_t head = th->accent;
    uint32_t torso = th->surface_2;
    uint32_t second = th->accent_2;
    uint32_t eye = th->bg;

    if (pet->style == UI_PET_BLOB) {
        torso = lerp_color(th->surface_2, th->accent, 35);
        second = th->accent;
    } else if (pet->style == UI_PET_BRASS) {
        head = th->accent_2;
        second = th->accent;
        torso = lerp_color(th->surface_2, th->accent, 14);
    } else if (pet->style == UI_PET_PULSE) {
        second = th->accent_2;
        torso = lerp_color(th->surface_2, th->accent_2, 24);
    }

    uint32_t tint = 0;
    int head_mix = 0, torso_mix = 0;
    state_tint(state, th, &tint, &head_mix, &torso_mix);
    head = lerp_color(head, tint, head_mix);
    torso = lerp_color(torso, tint, torso_mix);
    second = (state == UI_PET_OFFLINE) ? th->text_mut : second;
    eye = (state == UI_PET_OFFLINE) ? th->text_mut : eye;

    lv_obj_set_style_bg_color(pet->head, lv_color_hex(head), 0);
    lv_obj_set_style_bg_color(pet->torso, lv_color_hex(torso), 0);
    if (pet->ear_l) lv_obj_set_style_bg_color(pet->ear_l, lv_color_hex(second), 0);
    if (pet->ear_r) lv_obj_set_style_bg_color(pet->ear_r, lv_color_hex(second), 0);
    if (pet->tail) lv_obj_set_style_bg_color(pet->tail, lv_color_hex(second), 0);
    lv_obj_set_style_bg_color(pet->eye_l, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_color(pet->eye_r, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_color(pet->eye_closed_l, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_color(pet->eye_closed_r, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_color(pet->mouth, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_opa(pet->cheek_l,
                            state == UI_PET_OFFLINE ? LV_OPA_20 : LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(pet->cheek_r,
                            state == UI_PET_OFFLINE ? LV_OPA_20 : LV_OPA_60, 0);
    if (pet->aura) {
        lv_obj_set_style_bg_color(
            pet->aura,
            lv_color_hex(state == UI_PET_ALERT ? th->err : th->accent_2), 0);
        lv_obj_set_style_bg_opa(
            pet->aura, state == UI_PET_OFFLINE ? LV_OPA_TRANSP : LV_OPA_20, 0);
    }
}

void ui_pet_set_state(ui_pet_t *pet, int state)
{
    if (!pet || !pet->head || pet->state == state) return;
    pet->state = state;
    pet->t0 = lv_tick_get();
    if (pet->t0 == 0) pet->t0 = 1;
    set_palette(pet, state);

    int s = pet->size;
    const char *fx = "";
    bool closed = false;
    int mouth_w = s * 14 / 100;
    int mouth_h = s * 3 / 100;
    int mouth_radius = 2;
    int eye_w = s * 8 / 100;
    int eye_h = s * 11 / 100;

    switch (state) {
    case UI_PET_SLEEP:
        fx = "Zzz";
        closed = true;
        mouth_w = s * 10 / 100;
        mouth_h = 2;
        break;
    case UI_PET_HAPPY:
        fx = "+";
        mouth_w = s * 14 / 100;
        mouth_h = s * 9 / 100;
        mouth_radius = LV_RADIUS_CIRCLE;
        break;
    case UI_PET_BUSY:
        fx = "...";
        mouth_w = s * 10 / 100;
        mouth_h = 2;
        break;
    case UI_PET_ALERT:
        fx = "!";
        mouth_w = s * 8 / 100;
        mouth_h = s * 7 / 100;
        mouth_radius = LV_RADIUS_CIRCLE;
        eye_w = s * 10 / 100;
        eye_h = s * 14 / 100;
        break;
    case UI_PET_OFFLINE:
        fx = "?";
        closed = true;
        mouth_w = s * 8 / 100;
        mouth_h = 2;
        break;
    default:
        break;
    }

    lv_label_set_text(pet->fx, fx);
    lv_obj_set_style_opa(pet->fx, fx[0] ? LV_OPA_80 : LV_OPA_TRANSP, 0);
    show(pet->eye_closed_l, closed);
    show(pet->eye_closed_r, closed);
    show(pet->eye_l, !closed);
    show(pet->eye_r, !closed);
    pet->closed_visible = closed;

    place(pet->eye_l, s * 34 / 100, s * 31 / 100, eye_w, eye_h);
    place(pet->eye_r, s * 58 / 100, s * 31 / 100, eye_w, eye_h);
    place(pet->mouth, s / 2 - mouth_w / 2, s * 52 / 100, mouth_w, mouth_h);
    lv_obj_set_style_radius(pet->mouth, mouth_radius, 0);

    lv_obj_set_pos(pet->actor, pet->base_x, pet->base_y);
}

void ui_pet_tick(ui_pet_t *pet, uint32_t now_ms)
{
    if (!pet || !pet->actor) return;
    uint32_t t = now_ms - pet->t0;
    int dx = 0, dy = 0;
    int opa = 200;

    switch (pet->state) {
    case UI_PET_IDLE: {
        dy = (int)(lrintf(sinf((float)t / 900.0f) * 2.5f));
        bool blink = (t % 3600u) < 140u;
        if (blink != pet->closed_visible) {
            pet->closed_visible = blink;
            show(pet->eye_closed_l, blink);
            show(pet->eye_closed_r, blink);
            show(pet->eye_l, !blink);
            show(pet->eye_r, !blink);
        }
        break;
    }
    case UI_PET_HAPPY:
        dy = -(int)(fabsf(sinf((float)t / 300.0f)) * 6.0f);
        opa = (int)(160.0f + 80.0f * fabsf(sinf((float)t / 260.0f)));
        break;
    case UI_PET_BUSY:
        dx = (int)(sinf((float)t / 90.0f) * 2.0f);
        opa = (int)(120.0f + 80.0f * fabsf(sinf((float)t / 420.0f)));
        break;
    case UI_PET_ALERT:
        dx = ((t / 40u) % 2u) ? -1 : 1;
        opa = 230;
        break;
    case UI_PET_SLEEP:
        dx = (int)(sinf((float)t / 1200.0f) * 2.0f);
        opa = (int)(120.0f + 80.0f * fabsf(sinf((float)t / 1100.0f)));
        break;
    case UI_PET_OFFLINE:
        dy = 3;
        opa = 120;
        break;
    default:
        break;
    }

    if (pet->aura) {
        int aura_opa = (int)(10 + 14 * fabsf(sinf((float)t / 900.0f)));
        lv_obj_set_style_bg_opa(pet->aura, (lv_opa_t)aura_opa, 0);
    }
    if (pet->fx) {
        lv_obj_set_style_opa(pet->fx, (lv_opa_t)opa, 0);
    }
    lv_obj_set_pos(pet->actor, pet->base_x + dx, pet->base_y + dy);
}
