#include "ui.h"

lv_obj_t * ui_group_create(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, 0, 0);
    lv_obj_set_style_pad_column(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t * ui_card_create(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, UI_R_M, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_pad_all(o, 10, 0);
    lv_obj_set_style_pad_row(o, 2, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t * ui_label_create(lv_obj_t * parent, const char * text)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, UI_F_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
    return l;
}

lv_obj_t * ui_cjk_create(lv_obj_t * parent, const char * text)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
    return l;
}

lv_obj_t * ui_caption_create(lv_obj_t * parent, const char * text)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT_DIM), 0);
    return l;
}

lv_obj_t * ui_pill_create(lv_obj_t * parent, const char * text, uint32_t color)
{
    lv_obj_t * pill = ui_group_create(parent);
    lv_obj_set_style_bg_color(pill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    lv_obj_set_style_radius(pill, UI_R_PILL, 0);
    lv_obj_set_style_pad_hor(pill, 8, 0);
    lv_obj_set_style_pad_ver(pill, 3, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t * l = lv_label_create(pill);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return pill;
}

lv_obj_t * ui_bar_create(lv_obj_t * parent, int32_t max, uint32_t color)
{
    lv_obj_t * bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 5);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, UI_R_PILL, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, max > 0 ? max : 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

lv_obj_t * ui_chart_create(lv_obj_t * parent, int32_t height,
                           uint32_t color1, uint32_t color2,
                           lv_chart_series_t ** s1, lv_chart_series_t ** s2)
{
    lv_obj_t * chart = lv_chart_create(parent);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(UI_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DASH_HISTORY_LEN);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

    lv_chart_series_t * a = lv_chart_add_series(chart, lv_color_hex(color1),
                                                LV_CHART_AXIS_PRIMARY_Y);
    if (s1) *s1 = a;
    if (s2) {
        if (color2 != 0) {
            *s2 = lv_chart_add_series(chart, lv_color_hex(color2),
                                      LV_CHART_AXIS_SECONDARY_Y);
        } else {
            *s2 = NULL;
        }
    }
    return chart;
}
