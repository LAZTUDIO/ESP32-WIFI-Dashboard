/**
 * STRIX 主题 —— 480×320 横屏 / ROG 红黑斜切 / 电竞副屏格调
 *
 * 版式预算（内容 460×300）：
 *   顶栏 24 + 8 + 主区 150 + 8 + 底栏 68 + 8 + 导航 34 = 300
 */

#include "ui_theme.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SX_BG      0x050608u
#define SX_CARD    0x0C0F14u
#define SX_CARD2   0x12161Du
#define SX_BORDER  0x252C39u
#define SX_TEXT    0xF3F6FAu
#define SX_DIM     0xA2ADBDu
#define SX_MUT     0x7E8A9Cu
#define SX_RED     0xFF2A45u
#define SX_MAGENTA 0xFF3D7Fu
#define SX_CYAN    0x35E0FFu
#define SX_GREEN   0x3DE08Au
#define SX_AMBER   0xFFB020u

#define SX_PAD 10
#define SX_GAP 8

/* ============================== 状态 ============================== */

static lv_obj_t * s_root;
static lv_obj_t * s_pages[2];
static int s_page;
static lv_obj_t * s_nav_lbl[2];
static lv_obj_t * s_nav_btn[2];

static lv_obj_t * s_model;
static lv_obj_t * s_status_box;
static lv_obj_t * s_status_dot;
static lv_obj_t * s_status_lbl;
static lv_obj_t * s_clock;

static lv_obj_t * s_dec_val;
static lv_obj_t * s_dec_mode;
static lv_obj_t * s_dec_avg;
static lv_obj_t * s_dec_peak;
static lv_obj_t * s_spark;
static lv_chart_series_t * s_spark_ser;

static lv_obj_t * s_conc_val;
static lv_obj_t * s_conc_bar;
static lv_obj_t * s_queue_val;
static lv_obj_t * s_queue_bar;
static lv_obj_t * s_kv_val;
static lv_obj_t * s_kv_bar;
static lv_obj_t * s_cache_val;
static lv_obj_t * s_cache_bar;

static lv_obj_t * s_pre_now;
static lv_obj_t * s_pre_avg;
static lv_obj_t * s_pre_peak;
static lv_obj_t * s_pwr;
static lv_obj_t * s_tmp;
static lv_obj_t * s_gpu;
static lv_obj_t * s_ram;
static lv_obj_t * s_tokens;
static lv_obj_t * s_tokens_sub;

/* 设置页 */
static lv_obj_t * s_theme_pill[3];
static lv_obj_t * s_theme_lbl[3];
static lv_obj_t * s_rot_lbl;
static lv_obj_t * s_set_provider;
static lv_obj_t * s_set_endpoint;
static lv_obj_t * s_set_key;
static lv_obj_t * s_set_version;

/* ============================== 工具 ============================== */

/* 只有内容真的变了才写回：避免每 2 秒把所有标签重新 invalidate 一遍 */
static void txt(lv_obj_t * l, const char * t)
{
    if (!l || !t) return;
    const char * cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

static void col(lv_obj_t * l, uint32_t c)
{
    if (!l) return;
    lv_color_t cc = lv_color_hex(c);
    if (lv_color_to_u32(lv_obj_get_style_text_color(l, 0)) ==
        lv_color_to_u32(cc)) return;
    lv_obj_set_style_text_color(l, cc, 0);
}

static lv_obj_t * mk_label(lv_obj_t * parent, const char * t,
                           const lv_font_t * font, uint32_t c)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

static lv_obj_t * cap(lv_obj_t * parent, const char * t)
{
    lv_obj_t * l = mk_label(parent, t, UI_F_CAP, SX_MUT);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

static lv_obj_t * row(lv_obj_t * parent, int32_t h)
{
    lv_obj_t * g = ui_group_create(parent);
    lv_obj_set_size(g, LV_PCT(100), h);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return g;
}

static lv_obj_t * colbox(lv_obj_t * parent)
{
    lv_obj_t * g = ui_group_create(parent);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return g;
}

static lv_obj_t * card(lv_obj_t * parent)
{
    lv_obj_t * c = ui_card_create(parent);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(SX_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(SX_BORDER), 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    return c;
}

static lv_obj_t * accent_strip(lv_obj_t * parent, uint32_t c, int32_t w)
{
    lv_obj_t * s = ui_group_create(parent);
    lv_obj_set_size(s, w, 3);
    lv_obj_set_style_bg_color(s, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, 2, 0);
    return s;
}

static void bar_set(lv_obj_t * bar, int32_t max, int32_t v)
{
    if (!bar) return;
    int32_t mx = max > 0 ? max : 1;
    if (lv_bar_get_max_value(bar) == mx && lv_bar_get_value(bar) == v) return;
    lv_bar_set_range(bar, 0, mx);
    lv_bar_set_value(bar, v, LV_ANIM_OFF);
}

static void fmt_num(char * b, size_t n, float v, int prec)
{
    snprintf(b, n, "%.*f", prec, (double)v);
}

static void fmt_count(char * b, size_t n, uint64_t v)
{
    if (v >= 1000000ULL) snprintf(b, n, "%.2fM", (double)v / 1e6);
    else if (v >= 1000ULL) snprintf(b, n, "%.1fk", (double)v / 1e3);
    else snprintf(b, n, "%llu", (unsigned long long)v);
}

static float hist_max(const dash_history_t * h, const float * a)
{
    float m = 0;
    for (int i = 0; i < h->count; i++) if (a[i] > m) m = a[i];
    return m;
}

/* ============================== 组件 ============================== */

static lv_obj_t * micro_card(lv_obj_t * parent, const char * caption,
                             lv_obj_t ** value, lv_obj_t ** bar,
                             uint32_t color)
{
    lv_obj_t * c = card(parent);
    lv_obj_set_size(c, LV_PCT(100), 71);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_pad_left(c, 8, 0);

    cap(c, caption);

    lv_obj_t * v = mk_label(c, "--", &lv_font_montserrat_24, SX_TEXT);
    *value = v;

    if (bar) {
        lv_obj_t * b = ui_bar_create(c, 100, color);
        lv_obj_set_style_bg_color(b, lv_color_hex(SX_BORDER), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
        *bar = b;
    } else {
        *bar = NULL;
    }
    return c;
}

static lv_obj_t * stat_cell(lv_obj_t * parent, const char * caption,
                            lv_obj_t ** value, uint32_t color)
{
    lv_obj_t * g = colbox(parent);
    lv_obj_set_size(g, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(g, 1);
    lv_obj_set_style_pad_row(g, 0, 0);
    mk_label(g, caption, UI_F_CJK, SX_MUT);
    lv_obj_t * v = mk_label(g, "--", &lv_font_montserrat_20, color);
    *value = v;
    return g;
}

/** 遥测单行：PWR 132W，四行刚好填满 68px 卡片 */
static lv_obj_t * tel_row(lv_obj_t * parent, const char * caption,
                          lv_obj_t ** value, uint32_t color)
{
    lv_obj_t * g = ui_group_create(parent);
    lv_obj_set_size(g, LV_PCT(100), 14);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_label(g, caption, UI_F_CAP, SX_MUT);
    *value = mk_label(g, "--", UI_F_CAP, color);
    return g;
}

static void theme_pill_event(lv_event_t * e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    if (v == 0) dashboard_ui_emit(DASH_ACTION_THEME_CLASSIC);
    else if (v == 1) dashboard_ui_emit(DASH_ACTION_THEME_STRIX);
    else dashboard_ui_emit(DASH_ACTION_THEME_TUF);
}

static lv_obj_t * theme_pill(lv_obj_t * parent, const char * label,
                             int idx, bool active)
{
    lv_obj_t * b = ui_group_create(parent);
    lv_obj_set_size(b, LV_PCT(33) - 6, 30);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(SX_RED), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(SX_RED), 0);
    lv_obj_set_style_bg_opa(b, active ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, theme_pill_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);
    lv_obj_t * l = mk_label(b, label, UI_F_CJK, active ? SX_TEXT : SX_DIM);
    s_theme_pill[idx] = b;
    s_theme_lbl[idx] = l;
    return b;
}

static void action_btn_event(lv_event_t * e)
{
    dashboard_ui_emit((dash_action_t)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t * action_btn(lv_obj_t * parent, const char * text,
                             uint32_t c, dash_action_t a)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(c), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, action_btn_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)a);
    mk_label(b, text, UI_F_CJK, c);
    return b;
}

/* ============================== 页面 0 ============================== */

static void build_topbar(lv_obj_t * page)
{
    lv_obj_t * bar = row(page, 24);

    lv_obj_t * brand = ui_group_create(bar);
    lv_obj_set_size(brand, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brand, 6, 0);

    lv_obj_t * block = ui_group_create(brand);
    lv_obj_set_size(block, 4, 16);
    lv_obj_set_style_bg_color(block, lv_color_hex(SX_RED), 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);

    mk_label(brand, "ROG", &lv_font_montserrat_20, SX_TEXT);
    mk_label(brand, "// LLM TELEMETRY", UI_F_CAP, SX_MUT);

    lv_obj_t * mid = ui_group_create(bar);
    lv_obj_set_size(mid, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(mid, 1);

    s_model = mk_label(bar, "WAITING", UI_F_CAP, SX_DIM);
    lv_obj_set_size(s_model, 170, 18);
    lv_label_set_long_mode(s_model, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_model, LV_TEXT_ALIGN_RIGHT, 0);

    s_status_box = ui_group_create(bar);
    lv_obj_set_size(s_status_box, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_radius(s_status_box, 3, 0);
    lv_obj_set_style_bg_color(s_status_box, lv_color_hex(SX_MUT), 0);
    lv_obj_set_style_bg_opa(s_status_box, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(s_status_box, 6, 0);
    lv_obj_set_flex_flow(s_status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_status_box, 4, 0);

    s_status_dot = ui_group_create(s_status_box);
    lv_obj_set_size(s_status_dot, 5, 5);
    lv_obj_set_style_radius(s_status_dot, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(SX_MUT), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);

    s_status_lbl = mk_label(s_status_box, "离线", UI_F_CJK, SX_MUT);
}

static void build_hero(lv_obj_t * parent)
{
    lv_obj_t * c = card(parent);
    lv_obj_set_size(c, 208, 150);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_pad_row(c, 1, 0);
    accent_strip(c, SX_RED, 34);

    lv_obj_t * r1 = row(c, 18);
    cap(r1, "DECODE");
    s_dec_mode = mk_label(r1, "IDLE", UI_F_CAP, SX_MUT);

    lv_obj_t * r2 = ui_group_create(c);
    lv_obj_set_size(r2, LV_PCT(100), 52);
    lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(r2, 5, 0);
    s_dec_val = mk_label(r2, "--", &lv_font_montserrat_48, SX_TEXT);
    mk_label(r2, "tok/s", UI_F_CAP, SX_MUT);

    lv_obj_t * r3 = row(c, 20);
    lv_obj_t * g1 = ui_group_create(r3);
    lv_obj_set_size(g1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g1, 5, 0);
    mk_label(g1, "AVG", UI_F_CAP, SX_MUT);
    s_dec_avg = mk_label(g1, "--", UI_F_CAP, SX_DIM);

    lv_obj_t * g2 = ui_group_create(r3);
    lv_obj_set_size(g2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g2, 5, 0);
    mk_label(g2, "PEAK", UI_F_CAP, SX_MUT);
    s_dec_peak = mk_label(g2, "--", UI_F_CAP, SX_MAGENTA);

    s_spark = ui_chart_create(c, 32, SX_RED, 0, &s_spark_ser, NULL);
    lv_chart_set_div_line_count(s_spark, 2, 0);
    lv_chart_set_range(s_spark, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
}

static void build_bottom(lv_obj_t * page)
{
    lv_obj_t * bottom = ui_group_create(page);
    lv_obj_set_size(bottom, LV_PCT(100), 68);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bottom, SX_GAP, 0);

    /* 预填充：瞬时 / 均值 / 峰值 */
    lv_obj_t * pre = card(bottom);
    lv_obj_set_size(pre, 190, 68);
    lv_obj_set_style_pad_all(pre, 4, 0);
    lv_obj_set_style_pad_row(pre, 1, 0);
    cap(pre, "PREFILL");

    lv_obj_t * cells = ui_group_create(pre);
    lv_obj_set_size(cells, LV_PCT(100), 41);
    lv_obj_set_flex_flow(cells, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cells, 4, 0);
    stat_cell(cells, "当前", &s_pre_now, SX_TEXT);
    stat_cell(cells, "均值", &s_pre_avg, SX_DIM);
    stat_cell(cells, "峰值", &s_pre_peak, SX_CYAN);

    /* 硬件遥测：四行 */
    lv_obj_t * tel = card(bottom);
    lv_obj_set_size(tel, 138, 68);
    lv_obj_set_style_pad_all(tel, 6, 0);
    lv_obj_set_style_pad_row(tel, 0, 0);

    tel_row(tel, "PWR", &s_pwr, SX_AMBER);
    tel_row(tel, "TMP", &s_tmp, SX_RED);
    tel_row(tel, "GPU", &s_gpu, SX_GREEN);
    tel_row(tel, "RAM", &s_ram, SX_CYAN);

    /* 用量 */
    lv_obj_t * use = card(bottom);
    lv_obj_set_size(use, 116, 68);
    lv_obj_set_style_pad_all(use, 6, 0);
    cap(use, "TOKENS");
    s_tokens = mk_label(use, "--", &lv_font_montserrat_20, SX_TEXT);
    s_tokens_sub = mk_label(use, "--", UI_F_CAP, SX_MUT);
}

static void build_page_monitor(lv_obj_t * page)
{
    build_topbar(page);

    lv_obj_t * mainrow = ui_group_create(page);
    lv_obj_set_size(mainrow, LV_PCT(100), 150);
    lv_obj_set_flex_flow(mainrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mainrow, SX_GAP, 0);

    build_hero(mainrow);

    lv_obj_t * ca = colbox(mainrow);
    lv_obj_set_size(ca, 118, 150);
    lv_obj_set_style_pad_row(ca, SX_GAP, 0);
    micro_card(ca, "CONC", &s_conc_val, &s_conc_bar, SX_CYAN);
    micro_card(ca, "QUEUE", &s_queue_val, &s_queue_bar, SX_AMBER);

    lv_obj_t * cb = colbox(mainrow);
    lv_obj_set_size(cb, 118, 150);
    lv_obj_set_style_pad_row(cb, SX_GAP, 0);
    micro_card(cb, "KV CACHE", &s_kv_val, &s_kv_bar, SX_MAGENTA);
    micro_card(cb, "CACHE HIT", &s_cache_val, &s_cache_bar, SX_GREEN);

    build_bottom(page);
}

/* ============================== 页面 1（设置） ============================== */

static void build_page_settings(lv_obj_t * page)
{
    lv_obj_t * disp = card(page);
    lv_obj_set_size(disp, LV_PCT(100), 96);
    lv_obj_set_style_pad_all(disp, 8, 0);
    lv_obj_set_style_pad_row(disp, 3, 0);

    lv_obj_t * trow = row(disp, 16);
    cap(trow, "THEME / ORIENTATION");
    mk_label(trow, "立即", UI_F_CJK, SX_MUT);

    lv_obj_t * pills = ui_group_create(disp);
    lv_obj_set_size(pills, LV_PCT(100), 30);
    lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pills, 6, 0);
    theme_pill(pills, "CLASSIC", 0, false);
    theme_pill(pills, "STRIX", 1, true);
    theme_pill(pills, "TUF", 2, false);

    lv_obj_t * rrow = row(disp, 28);
    lv_obj_t * left = ui_group_create(rrow);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(left, 8, 0);
    mk_label(left, "屏幕方向", UI_F_CJK, SX_MUT);
    s_rot_lbl = mk_label(left, "0", UI_F_CAP, SX_CYAN);

    lv_obj_t * rotbtn = lv_button_create(rrow);
    lv_obj_remove_style_all(rotbtn);
    lv_obj_set_size(rotbtn, 88, 26);
    lv_obj_set_style_bg_color(rotbtn, lv_color_hex(SX_CYAN), 0);
    lv_obj_set_style_bg_opa(rotbtn, LV_OPA_20, 0);
    lv_obj_set_style_radius(rotbtn, 4, 0);
    lv_obj_set_style_border_width(rotbtn, 1, 0);
    lv_obj_set_style_border_color(rotbtn, lv_color_hex(SX_CYAN), 0);
    lv_obj_set_flex_flow(rotbtn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rotbtn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(rotbtn, action_btn_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)DASH_ACTION_TOGGLE_ROT180);
    mk_label(rotbtn, "180", UI_F_CJK, SX_CYAN);

    /* 连接信息 */
    lv_obj_t * conn = card(page);
    lv_obj_set_size(conn, LV_PCT(100), 112);
    lv_obj_set_style_pad_all(conn, 8, 0);
    lv_obj_set_style_pad_row(conn, 0, 0);
    lv_obj_t * ch = row(conn, 16);
    cap(ch, "ENDPOINT");

    const char * keys[4] = {"服务", "地址", "密钥", "版本"};
    lv_obj_t ** vals[4] = {&s_set_provider, &s_set_endpoint, &s_set_key,
                           &s_set_version};
    for (int i = 0; i < 4; i++) {
        lv_obj_t * r = row(conn, 20);
        mk_label(r, keys[i], UI_F_CJK, SX_MUT);
        lv_obj_t * v = mk_label(r, "--", UI_F_CJK, i == 0 ? SX_RED : SX_DIM);
        lv_obj_set_size(v, 330, 20);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        *vals[i] = v;
    }

    lv_obj_t * acts = ui_group_create(page);
    lv_obj_set_size(acts, LV_PCT(100), 34);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(acts, SX_GAP, 0);
    action_btn(acts, "立即刷新", SX_CYAN, DASH_ACTION_REFRESH);
    lv_obj_set_size(lv_obj_get_child(acts, 0), LV_PCT(33) - 6, 34);
    action_btn(acts, "重新配网", SX_RED, DASH_ACTION_PROVISION);
    lv_obj_set_size(lv_obj_get_child(acts, 1), LV_PCT(33) - 6, 34);
    action_btn(acts, "重启设备", SX_AMBER, DASH_ACTION_REBOOT);
    lv_obj_set_size(lv_obj_get_child(acts, 2), LV_PCT(33) - 6, 34);
}

/* ============================== 导航 ============================== */

static void nav_event(lv_event_t * e)
{
    ui_strix_set_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_nav(lv_obj_t * root)
{
    lv_obj_t * nav = ui_group_create(root);
    lv_obj_set_size(nav, LV_PCT(100), 34);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 6, 0);

    const char * names[2] = {"监控", "设置"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t * b = ui_group_create(nav);
        lv_obj_set_size(b, 130, 30);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(SX_RED), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(SX_RED), 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_nav_lbl[i] = mk_label(b, names[i], UI_F_CJK, SX_DIM);
        s_nav_btn[i] = b;
    }

    lv_obj_t * rest = ui_group_create(nav);
    lv_obj_set_size(rest, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(rest, 1);

    lv_obj_t * foot = ui_group_create(nav);
    lv_obj_set_size(foot, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_clock = mk_label(foot, "--:--", UI_F_CAP, SX_DIM);
}

/* ============================== 创建 ============================== */

lv_obj_t * ui_strix_create(void)
{
    s_page = 0;

    /* 独立 screen：未加载时完全不参与渲染，不会影响经典页 */
    s_root = ui_group_create(NULL);
    lv_obj_set_size(s_root, UI_STRIX_W, UI_STRIX_H);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(SX_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, SX_PAD, 0);
    lv_obj_set_style_pad_row(s_root, SX_GAP, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t * pages = ui_group_create(s_root);
    lv_obj_set_size(pages, LV_PCT(100), 258);
    lv_obj_set_style_bg_opa(pages, LV_OPA_TRANSP, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t * p = ui_group_create(pages);
        lv_obj_set_size(p, LV_PCT(100), 258);
        lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(p, SX_GAP, 0);
        s_pages[i] = p;
    }
    build_page_monitor(s_pages[0]);
    build_page_settings(s_pages[1]);

    build_nav(s_root);
    ui_strix_set_page(0);
    return s_root;
}

void ui_strix_set_page(int page)
{
    if (page < 0 || page > 1) return;
    s_page = page;
    for (int i = 0; i < 2; i++) {
        if (i == page) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        bool active = (i == page);
        lv_obj_set_style_bg_opa(s_nav_btn[i], active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_nav_btn[i], active ? 1 : 0, 0);
        col(s_nav_lbl[i], active ? SX_RED : SX_DIM);
    }
}

int ui_strix_page_count(void)
{
    return 2;
}

/* ============================== 更新 ============================== */

static void set_status(uint32_t c, const char * t)
{
    if (s_status_lbl && strcmp(lv_label_get_text(s_status_lbl), t) == 0 &&
        lv_color_to_u32(lv_obj_get_style_text_color(s_status_lbl, 0)) ==
        lv_color_to_u32(lv_color_hex(c)) &&
        (!s_status_box ||
         lv_color_to_u32(lv_obj_get_style_bg_color(s_status_box, 0)) ==
         lv_color_to_u32(lv_color_hex(c)))) {
        return;
    }
    if (s_status_box) {
        lv_obj_set_style_bg_color(s_status_box, lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(s_status_box, LV_OPA_20, 0);
    }
    col(s_status_lbl, c);
    if (s_status_dot) {
        lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(c), 0);
    }
    txt(s_status_lbl, t);
}

static float dec_scale(const dash_history_t * h)
{
    float mx = hist_max(h, h->tps_dec);
    return mx < 10.0f ? 10.0f : mx * 1.2f;
}

void ui_strix_apply(const dash_metrics_t * m, const dash_history_t * h,
                    bool new_sample)
{
    char b[64];
    int pct;

    /* 时钟 / 方向 */
    if (s_clock) {
        time_t now = time(NULL);
        struct tm tm;
        if (now > 1700000000 && localtime_r(&now, &tm)) {
            strftime(b, sizeof(b), "%H:%M", &tm);
            txt(s_clock, b);
        }
    }
    txt(s_rot_lbl, dashboard_ui_rot180() ? "180" : "0");

    /* 顶栏 */
    txt(s_model, m->model[0] ? m->model : "WAITING");
    if (!m->online) set_status(SX_RED, m->stale ? "重连" : "离线");
    else if (m->busy) set_status(SX_CYAN, "解码");
    else set_status(SX_GREEN, "在线");

    /* 解码主值 */
    if (m->has_tps && m->online) {
        fmt_num(b, sizeof(b), m->tps_dec, m->tps_dec >= 100 ? 0 : 1);
        txt(s_dec_val, b);
        col(s_dec_val, SX_TEXT);
        txt(s_dec_mode, m->tps_live ? "LIVE" : "LAST");
        col(s_dec_mode, m->tps_live ? SX_GREEN : SX_AMBER);

        fmt_num(b, sizeof(b), m->tps_avg, 1);
        txt(s_dec_avg, b);
        fmt_num(b, sizeof(b), m->tps_peak, 1);
        txt(s_dec_peak, b);
    } else {
        txt(s_dec_val, "--");
        col(s_dec_val, SX_MUT);
        txt(s_dec_mode, m->online ? "NODATA" : "OFFLINE");
        col(s_dec_mode, SX_MUT);
        txt(s_dec_avg, "--");
        txt(s_dec_peak, "--");
    }

    /* 并发 / 排队 */
    if (m->has_concurrency) {
        snprintf(b, sizeof(b), "%u/%u", (unsigned)m->in_flight,
                 (unsigned)m->slots);
        txt(s_conc_val, b);
        col(s_conc_val, m->in_flight ? SX_CYAN : SX_TEXT);
        bar_set(s_conc_bar, m->slots, m->in_flight);
    } else {
        txt(s_conc_val, "--");
        col(s_conc_val, SX_MUT);
    }

    if (m->has_queue) {
        snprintf(b, sizeof(b), "%u", (unsigned)m->queued);
        txt(s_queue_val, b);
        col(s_queue_val, m->queued ? SX_AMBER : SX_TEXT);
        bar_set(s_queue_bar, 10, m->queued);
    } else {
        txt(s_queue_val, "--");
        col(s_queue_val, SX_MUT);
    }

    /* KV / 缓存 */
    if (m->has_kv) {
        snprintf(b, sizeof(b), "%.0f%%", (double)m->kv_pct);
        txt(s_kv_val, b);
        col(s_kv_val, m->kv_pct >= 85 ? SX_RED :
            (m->kv_pct >= 65 ? SX_AMBER : SX_MAGENTA));
        bar_set(s_kv_bar, 100, (int32_t)(m->kv_pct + 0.5f));
    } else {
        txt(s_kv_val, "--");
        col(s_kv_val, SX_MUT);
    }

    if (m->has_cache) {
        snprintf(b, sizeof(b), "%.0f%%", (double)m->cache_hit_pct);
        txt(s_cache_val, b);
        col(s_cache_val, m->cache_hit_pct >= 60 ? SX_GREEN : SX_AMBER);
        bar_set(s_cache_bar, 100, (int32_t)(m->cache_hit_pct + 0.5f));
    } else {
        txt(s_cache_val, "--");
        col(s_cache_val, SX_MUT);
    }

    /* 预填充三值 */
    if (m->has_tps && (m->tps_peak_pre > 0 || m->tps_avg_pre > 0)) {
        if (m->tps_pre > 0) {
            fmt_num(b, sizeof(b), m->tps_pre, m->tps_pre >= 100 ? 0 : 1);
            txt(s_pre_now, b);
        } else {
            txt(s_pre_now, "--");
        }
        fmt_num(b, sizeof(b), m->tps_avg_pre, m->tps_avg_pre >= 100 ? 0 : 1);
        txt(s_pre_avg, b);
        fmt_num(b, sizeof(b), m->tps_peak_pre, 0);
        txt(s_pre_peak, b);
    } else {
        txt(s_pre_now, "--");
        txt(s_pre_avg, "--");
        txt(s_pre_peak, "--");
    }

    /* 硬件 */
    if (m->has_power) {
        snprintf(b, sizeof(b), "%.0fW", (double)m->power_w);
        txt(s_pwr, b);
        col(s_pwr, m->power_w > 200 ? SX_RED : SX_AMBER);
    } else { txt(s_pwr, "--"); col(s_pwr, SX_MUT); }

    if (m->has_temp) {
        snprintf(b, sizeof(b), "%.0fC", (double)m->temp_c);
        txt(s_tmp, b);
        col(s_tmp, m->temp_c >= 80 ? SX_RED : SX_TEXT);
    } else { txt(s_tmp, "--"); col(s_tmp, SX_MUT); }

    if (m->has_gpu) {
        snprintf(b, sizeof(b), "%.0f%%", (double)m->gpu_pct);
        txt(s_gpu, b);
    } else { txt(s_gpu, "--"); }

    if (m->has_ram && m->ram_total_mb) {
        pct = (int)(100.0f * (float)m->ram_used_mb / (float)m->ram_total_mb);
        snprintf(b, sizeof(b), "%d%%", pct);
        txt(s_ram, b);
    } else if (m->has_vram && m->vram_total_mb) {
        pct = (int)(100.0f * (float)m->vram_used_mb / (float)m->vram_total_mb);
        snprintf(b, sizeof(b), "%d%%", pct);
        txt(s_ram, b);
    } else { txt(s_ram, "--"); }

    /* 用量 */
    if (m->has_usage) {
        char in[16], out[16];
        fmt_count(in, sizeof(in), m->tokens_in);
        fmt_count(out, sizeof(out), m->tokens_out);
        fmt_count(b, sizeof(b), m->tokens_in + m->tokens_out);
        txt(s_tokens, b);
        snprintf(b, sizeof(b), "%s / %s", in, out);
        txt(s_tokens_sub, b);
    } else {
        txt(s_tokens, "--");
        txt(s_tokens_sub, "--");
    }

    /* 设置页 */
    txt(s_set_provider, m->provider_name[0] ? m->provider_name : "自动识别");
    txt(s_set_endpoint, m->endpoint[0] ? m->endpoint : "未设置");
    txt(s_set_key, m->api_key_mask[0] ? m->api_key_mask : "未设置");
    if (m->version[0]) {
        snprintf(b, sizeof(b), "v%s", m->version);
        txt(s_set_version, b);
    } else {
        txt(s_set_version, "--");
    }

    /* sparkline */
    if (new_sample && h->count > 0) {
        lv_chart_set_next_value(s_spark, s_spark_ser,
                                (int32_t)(h->tps_dec[h->count - 1] * 10.0f + 0.5f));
        lv_chart_set_range(s_spark, LV_CHART_AXIS_PRIMARY_Y, 0,
                           (int32_t)(dec_scale(h) * 10.0f + 0.5f));
        lv_chart_refresh(s_spark);
    }
}
