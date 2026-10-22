/**
 * TUF 主题 —— 320×480 竖屏 / 石墨 + 琥珀军工风
 *
 * 版式预算（内容 304×464）：
 *   页面区 416 + 8 + 导航 40 = 464
 *   监控页：头部 34 + 6 + 主卡 116 + 6 + 预填充 90 + 6 + 趋势 104 + 6 + 遥测 48 = 416
 */

#include "ui_theme.h"

#include <stdio.h>
#include <string.h>

#define TF_BG      0x0B0C0Au
#define TF_CARD    0x151713u
#define TF_CARD2   0x1B1E17u
#define TF_BORDER  0x33382Cu
#define TF_TEXT    0xF2F4ECu
#define TF_DIM     0xAAB19Fu
#define TF_MUT     0x848C77u
#define TF_AMBER   0xFFB000u
#define TF_LIME    0xC6F24Au
#define TF_RED     0xFF5A4Au
#define TF_STEEL   0x9FB3C8u

#define TF_PAD 8
#define TF_GAP 6

/* ============================== 状态 ============================== */

static lv_obj_t * s_root;
static lv_obj_t * s_pages[3];
static int s_page;
static lv_obj_t * s_nav_btn[3];
static lv_obj_t * s_nav_lbl[3];

static lv_obj_t * s_model;
static lv_obj_t * s_status_box;
static lv_obj_t * s_status_lbl;
static lv_obj_t * s_status_dot;

static lv_obj_t * s_dec_now;
static lv_obj_t * s_dec_avg;
static lv_obj_t * s_dec_peak;
static lv_obj_t * s_dec_mode;
static lv_obj_t * s_seg[10];
static int s_seg_last = -1;

static lv_obj_t * s_pre_now;
static lv_obj_t * s_pre_avg;
static lv_obj_t * s_pre_peak;

static lv_obj_t * s_chart;
static lv_chart_series_t * s_ser1;
static lv_chart_series_t * s_ser2;

static lv_obj_t * s_pwr;
static lv_obj_t * s_tmp;
static lv_obj_t * s_gpu;
static lv_obj_t * s_ram;

/* 趋势页 */
static lv_obj_t * s_trend_chart;
static lv_chart_series_t * s_trend1;
static lv_chart_series_t * s_trend2;
static lv_obj_t * s_trend_now;
static lv_obj_t * s_trend_avg;
static lv_obj_t * s_trend_peak;
static lv_obj_t * s_trend_pre;
static lv_obj_t * s_stat_req;

/* 设置页 */
static lv_obj_t * s_theme_btn[3];
static lv_obj_t * s_theme_lbl[3];
static lv_obj_t * s_rot_lbl;
static lv_obj_t * s_set_provider;
static lv_obj_t * s_set_endpoint;
static lv_obj_t * s_set_key;
static lv_obj_t * s_about_model;
static lv_obj_t * s_about_up;

/* ============================== 工具 ============================== */

/* 只有内容真的变了才写回：避免无变化的标签反复 invalidate */
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
    lv_obj_t * l = mk_label(parent, t, UI_F_CAP, TF_MUT);
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

static lv_obj_t * card(lv_obj_t * parent)
{
    lv_obj_t * c = ui_card_create(parent);
    lv_obj_set_style_radius(c, 3, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(TF_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(TF_BORDER), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    return c;
}

/** 顶部警示条：交替琥珀/透明块，TUF 的标志性细节 */
static lv_obj_t * hazard_strip(lv_obj_t * parent)
{
    lv_obj_t * g = ui_group_create(parent);
    lv_obj_set_size(g, LV_PCT(100), 4);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g, 2, 0);
    for (int i = 0; i < 12; i++) {
        lv_obj_t * b = ui_group_create(g);
        lv_obj_set_size(b, 0, 4);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, lv_color_hex(TF_AMBER), 0);
        lv_obj_set_style_bg_opa(b, (i % 3 == 0) ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    return g;
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

static float hist_avg(const dash_history_t * h, const float * a)
{
    if (h->count == 0) return 0;
    float sum = 0;
    for (int i = 0; i < h->count; i++) sum += a[i];
    return sum / (float)h->count;
}

/* ============================== 组件 ============================== */

static lv_obj_t * stat_cell(lv_obj_t * parent, const char * caption,
                            lv_obj_t ** value, uint32_t color,
                            const lv_font_t * vfont)
{
    lv_obj_t * g = ui_group_create(parent);
    lv_obj_set_size(g, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(g, 1);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g, 0, 0);
    mk_label(g, caption, UI_F_CJK, TF_MUT);
    *value = mk_label(g, "--", vfont, color);
    return g;
}

static void theme_event_cb(lv_event_t * e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    if (v == 0) dashboard_ui_emit(DASH_ACTION_THEME_CLASSIC);
    else if (v == 1) dashboard_ui_emit(DASH_ACTION_THEME_STRIX);
    else dashboard_ui_emit(DASH_ACTION_THEME_TUF);
}

static lv_obj_t * theme_btn(lv_obj_t * parent, const char * label,
                            int idx, bool active)
{
    lv_obj_t * b = ui_group_create(parent);
    lv_obj_set_size(b, LV_PCT(33) - 5, 30);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(TF_AMBER), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TF_AMBER), 0);
    lv_obj_set_style_bg_opa(b, active ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, theme_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);
    s_theme_btn[idx] = b;
    s_theme_lbl[idx] = mk_label(b, label, UI_F_CJK, active ? TF_TEXT : TF_DIM);
    return b;
}

static void action_cb(lv_event_t * e)
{
    dashboard_ui_emit((dash_action_t)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t * action_btn(lv_obj_t * parent, const char * text,
                             uint32_t c, dash_action_t a)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(33) - 5, 36);
    lv_obj_set_style_bg_color(b, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(c), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    mk_label(b, text, UI_F_CJK, c);
    return b;
}

/* ============================== 头部 ============================== */

static void build_header(lv_obj_t * page)
{
    lv_obj_t * head = row(page, 34);

    lv_obj_t * left = ui_group_create(head);
    lv_obj_set_size(left, LV_SIZE_CONTENT, 34);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(left, 0, 0);

    lv_obj_t * brand = ui_group_create(left);
    lv_obj_set_size(brand, LV_SIZE_CONTENT, 18);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brand, 5, 0);
    lv_obj_t * tag = ui_group_create(brand);
    lv_obj_set_size(tag, 30, 16);
    lv_obj_set_style_bg_color(tag, lv_color_hex(TF_AMBER), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tag, 2, 0);
    lv_obj_set_flex_flow(tag, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tag, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t * tl = mk_label(tag, "TUF", UI_F_CAP, 0x16180Fu);
    lv_obj_set_style_text_letter_space(tl, 1, 0);
    mk_label(brand, "LLM DECK", UI_F_CAP, TF_DIM);

    s_model = mk_label(left, "WAITING", UI_F_CAP, TF_MUT);
    lv_obj_set_size(s_model, 210, 16);
    lv_label_set_long_mode(s_model, LV_LABEL_LONG_DOT);

    s_status_box = ui_group_create(head);
    lv_obj_set_size(s_status_box, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_radius(s_status_box, 2, 0);
    lv_obj_set_style_bg_color(s_status_box, lv_color_hex(TF_MUT), 0);
    lv_obj_set_style_bg_opa(s_status_box, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(s_status_box, 6, 0);
    lv_obj_set_flex_flow(s_status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_status_box, 4, 0);
    s_status_dot = ui_group_create(s_status_box);
    lv_obj_set_size(s_status_dot, 5, 5);
    lv_obj_set_style_radius(s_status_dot, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(TF_MUT), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    s_status_lbl = mk_label(s_status_box, "离线", UI_F_CJK, TF_MUT);
}

/* ============================== 监控页 ============================== */

static void build_hero(lv_obj_t * page)
{
    lv_obj_t * c = card(page);
    lv_obj_set_size(c, LV_PCT(100), 116);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    lv_obj_t * top = row(c, 16);
    cap(top, "DECODE THROUGHPUT");
    s_dec_mode = mk_label(top, "IDLE", UI_F_CAP, TF_MUT);

    lv_obj_t * val = ui_group_create(c);
    lv_obj_set_size(val, LV_PCT(100), 48);
    lv_obj_set_flex_flow(val, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(val, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(val, 5, 0);
    s_dec_now = mk_label(val, "--", &lv_font_montserrat_40, TF_TEXT);
    mk_label(val, "tok/s", UI_F_CAP, TF_MUT);

    lv_obj_t * sub = ui_group_create(c);
    lv_obj_set_size(sub, LV_PCT(100), 16);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sub, 10, 0);
    lv_obj_t * g1 = ui_group_create(sub);
    lv_obj_set_size(g1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g1, 4, 0);
    mk_label(g1, "AVG", UI_F_CAP, TF_MUT);
    s_dec_avg = mk_label(g1, "--", UI_F_CAP, TF_DIM);
    lv_obj_t * g2 = ui_group_create(sub);
    lv_obj_set_size(g2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g2, 4, 0);
    mk_label(g2, "PEAK", UI_F_CAP, TF_MUT);
    s_dec_peak = mk_label(g2, "--", UI_F_CAP, TF_AMBER);

    /* 10 段刻度条 */
    lv_obj_t * seg = ui_group_create(c);
    lv_obj_set_size(seg, LV_PCT(100), 6);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 2, 0);
    for (int i = 0; i < 10; i++) {
        lv_obj_t * b = ui_group_create(seg);
        lv_obj_set_size(b, 0, 6);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, lv_color_hex(TF_BORDER), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        s_seg[i] = b;
    }
}

static void build_prefill(lv_obj_t * page)
{
    lv_obj_t * c = card(page);
    lv_obj_set_size(c, LV_PCT(100), 90);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    lv_obj_t * top = row(c, 16);
    cap(top, "PREFILL");
    mk_label(top, "tok/s", UI_F_CAP, TF_MUT);

    lv_obj_t * cells = ui_group_create(c);
    lv_obj_set_size(cells, LV_PCT(100), 46);
    lv_obj_set_flex_flow(cells, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cells, 4, 0);
    stat_cell(cells, "当前", &s_pre_now, TF_TEXT, &lv_font_montserrat_24);
    stat_cell(cells, "均值", &s_pre_avg, TF_DIM, &lv_font_montserrat_24);
    stat_cell(cells, "峰值", &s_pre_peak, TF_LIME, &lv_font_montserrat_24);
}

static void build_trend(lv_obj_t * page)
{
    lv_obj_t * c = card(page);
    lv_obj_set_size(c, LV_PCT(100), 104);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    lv_obj_t * top = row(c, 18);
    cap(top, "THROUGHPUT");
    lv_obj_t * lg = ui_group_create(top);
    lv_obj_set_size(lg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(lg, 8, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t * g = ui_group_create(lg);
        lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(g, 3, 0);
        lv_obj_t * d = ui_group_create(g);
        lv_obj_set_size(d, 5, 5);
        lv_obj_set_style_radius(d, UI_R_PILL, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(i ? TF_LIME : TF_AMBER), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        mk_label(g, i ? "预填" : "解码", UI_F_CJK, TF_MUT);
    }

    s_chart = ui_chart_create(c, 66, TF_AMBER, TF_LIME, &s_ser1, &s_ser2);
    lv_chart_set_div_line_count(s_chart, 3, 0);
}

static void build_telemetry(lv_obj_t * page)
{
    lv_obj_t * c = card(page);
    lv_obj_set_size(c, LV_PCT(100), 48);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(c, 4, 0);

    stat_cell(c, "PWR", &s_pwr, TF_AMBER, &lv_font_montserrat_20);
    stat_cell(c, "TEMP", &s_tmp, TF_RED, &lv_font_montserrat_20);
    stat_cell(c, "GPU", &s_gpu, TF_LIME, &lv_font_montserrat_20);
    stat_cell(c, "RAM", &s_ram, TF_STEEL, &lv_font_montserrat_20);
}

static void build_page_monitor(lv_obj_t * page)
{
    build_hero(page);
    build_prefill(page);
    build_trend(page);
    build_telemetry(page);
}

/* ============================== 趋势页 ============================== */

static void build_page_trend(lv_obj_t * page)
{
    lv_obj_t * c = card(page);
    lv_obj_set_size(c, LV_PCT(100), 252);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    lv_obj_t * top = row(c, 18);
    cap(top, "DECODE / PREFILL");
    mk_label(top, "60 samples", UI_F_CAP, TF_MUT);
    hazard_strip(c);

    s_trend_chart = ui_chart_create(c, 204, TF_AMBER, TF_LIME,
                                    &s_trend1, &s_trend2);
    lv_chart_set_div_line_count(s_trend_chart, 4, 0);

    lv_obj_t * stats = ui_group_create(page);
    lv_obj_set_size(stats, LV_PCT(100), 118);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stats, 6, 0);

    /* 左列：解码三值 + 预填充 */
    lv_obj_t * left = ui_group_create(stats);
    lv_obj_set_size(left, LV_PCT(50) - 3, 118);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 6, 0);

    lv_obj_t * r1 = card(left);
    lv_obj_set_size(r1, LV_PCT(100), 56);
    lv_obj_set_style_pad_all(r1, 6, 0);
    lv_obj_set_style_pad_row(r1, 0, 0);
    cap(r1, "DECODE NOW");
    s_trend_now = mk_label(r1, "--", &lv_font_montserrat_24, TF_TEXT);

    lv_obj_t * r2 = card(left);
    lv_obj_set_size(r2, LV_PCT(100), 56);
    lv_obj_set_style_pad_all(r2, 6, 0);
    lv_obj_set_style_pad_row(r2, 0, 0);
    cap(r2, "AVG / PEAK");
    lv_obj_t * r2v = ui_group_create(r2);
    lv_obj_set_size(r2v, LV_PCT(100), 24);
    lv_obj_set_flex_flow(r2v, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r2v, 8, 0);
    s_trend_avg = mk_label(r2v, "--", &lv_font_montserrat_20, TF_DIM);
    s_trend_peak = mk_label(r2v, "--", &lv_font_montserrat_20, TF_AMBER);

    /* 右列：预填充 + 累计请求 */
    lv_obj_t * right = ui_group_create(stats);
    lv_obj_set_size(right, LV_PCT(50) - 3, 118);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, 0);

    lv_obj_t * r3 = card(right);
    lv_obj_set_size(r3, LV_PCT(100), 56);
    lv_obj_set_style_pad_all(r3, 6, 0);
    lv_obj_set_style_pad_row(r3, 0, 0);
    cap(r3, "PREFILL");
    lv_obj_t * r3v = ui_group_create(r3);
    lv_obj_set_size(r3v, LV_PCT(100), 24);
    lv_obj_set_flex_flow(r3v, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r3v, 8, 0);
    s_trend_pre = mk_label(r3v, "--", &lv_font_montserrat_20, TF_LIME);

    lv_obj_t * r4 = card(right);
    lv_obj_set_size(r4, LV_PCT(100), 56);
    lv_obj_set_style_pad_all(r4, 6, 0);
    lv_obj_set_style_pad_row(r4, 0, 0);
    cap(r4, "REQUESTS");
    s_stat_req = mk_label(r4, "--", &lv_font_montserrat_24, TF_TEXT);

}

/* ============================== 设置页 ============================== */

static void build_page_settings(lv_obj_t * page)
{
    lv_obj_t * display = card(page);
    lv_obj_set_size(display, LV_PCT(100), 100);
    lv_obj_set_style_pad_all(display, 8, 0);
    lv_obj_set_style_pad_row(display, 4, 0);

    lv_obj_t * top = row(display, 16);
    cap(top, "THEME / ORIENTATION");
    mk_label(top, "立即", UI_F_CJK, TF_MUT);

    lv_obj_t * pills = ui_group_create(display);
    lv_obj_set_size(pills, LV_PCT(100), 30);
    lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pills, 6, 0);
    theme_btn(pills, "CLASSIC", 0, false);
    theme_btn(pills, "STRIX", 1, false);
    theme_btn(pills, "TUF", 2, true);

    lv_obj_t * rrow = row(display, 28);
    lv_obj_t * lft = ui_group_create(rrow);
    lv_obj_set_size(lft, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lft, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(lft, 6, 0);
    mk_label(lft, "屏幕方向", UI_F_CJK, TF_MUT);
    s_rot_lbl = mk_label(lft, "0", UI_F_CAP, TF_LIME);

    lv_obj_t * rot = lv_button_create(rrow);
    lv_obj_remove_style_all(rot);
    lv_obj_set_size(rot, 96, 26);
    lv_obj_set_style_bg_color(rot, lv_color_hex(TF_AMBER), 0);
    lv_obj_set_style_bg_opa(rot, LV_OPA_20, 0);
    lv_obj_set_style_radius(rot, 3, 0);
    lv_obj_set_style_border_width(rot, 1, 0);
    lv_obj_set_style_border_color(rot, lv_color_hex(TF_AMBER), 0);
    lv_obj_set_flex_flow(rot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(rot, action_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)DASH_ACTION_TOGGLE_ROT180);
    mk_label(rot, "180", UI_F_CJK, TF_AMBER);

    /* 连接 */
    lv_obj_t * conn = card(page);
    lv_obj_set_size(conn, LV_PCT(100), 96);
    lv_obj_set_style_pad_all(conn, 8, 0);
    lv_obj_set_style_pad_row(conn, 0, 0);
    lv_obj_t * ch = row(conn, 16);
    cap(ch, "ENDPOINT");
    const char * keys[3] = {"服务", "地址", "密钥"};
    lv_obj_t ** vals[3] = {&s_set_provider, &s_set_endpoint, &s_set_key};
    for (int i = 0; i < 3; i++) {
        lv_obj_t * r = row(conn, 20);
        mk_label(r, keys[i], UI_F_CJK, TF_MUT);
        lv_obj_t * v = mk_label(r, "--", UI_F_CJK, i == 0 ? TF_AMBER : TF_DIM);
        lv_obj_set_size(v, 210, 20);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        *vals[i] = v;
    }

    /* 关于 */
    lv_obj_t * about = card(page);
    lv_obj_set_size(about, LV_PCT(100), 80);
    lv_obj_set_style_pad_all(about, 8, 0);
    lv_obj_set_style_pad_row(about, 0, 0);
    lv_obj_t * ah = row(about, 16);
    cap(ah, "DEVICE");
    lv_obj_t * a1 = row(about, 20);
    mk_label(a1, "模型", UI_F_CJK, TF_MUT);
    s_about_model = mk_label(a1, "--", UI_F_CJK, TF_TEXT);
    lv_obj_set_size(s_about_model, 190, 20);
    lv_label_set_long_mode(s_about_model, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_about_model, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t * a2 = row(about, 20);
    mk_label(a2, "运行", UI_F_CJK, TF_MUT);
    s_about_up = mk_label(a2, "--", UI_F_CJK, TF_DIM);
    lv_obj_set_size(s_about_up, 190, 20);
    lv_obj_set_style_text_align(s_about_up, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t * acts = ui_group_create(page);
    lv_obj_set_size(acts, LV_PCT(100), 36);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(acts, 6, 0);
    action_btn(acts, "刷新", TF_LIME, DASH_ACTION_REFRESH);
    action_btn(acts, "配网", TF_AMBER, DASH_ACTION_PROVISION);
    action_btn(acts, "重启", TF_RED, DASH_ACTION_REBOOT);
}

/* ============================== 导航 ============================== */

static void nav_cb(lv_event_t * e)
{
    ui_tuf_set_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_nav(lv_obj_t * root)
{
    lv_obj_t * nav = ui_group_create(root);
    lv_obj_set_size(nav, LV_PCT(100), 40);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 6, 0);

    const char * names[3] = {"监控", "曲线", "设置"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t * b = ui_group_create(nav);
        lv_obj_set_size(b, LV_PCT(33) - 4, 36);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(TF_AMBER), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(TF_AMBER), 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_nav_btn[i] = b;
        s_nav_lbl[i] = mk_label(b, names[i], UI_F_CJK, TF_DIM);
    }
}

/* ============================== 创建 ============================== */

lv_obj_t * ui_tuf_create(void)
{
    s_page = 0;
    s_seg_last = -1;
    /* 独立 screen：按需加载，避免多棵树同时参与刷新 */
    s_root = ui_group_create(NULL);
    lv_obj_set_size(s_root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(TF_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, TF_PAD, 0);
    lv_obj_set_style_pad_row(s_root, 6, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    /* 头部放在页面外：三页共用，避免重复创建导致指针被覆盖 */
    build_header(s_root);

    lv_obj_t * pages = ui_group_create(s_root);
    lv_obj_set_size(pages, LV_PCT(100), 376);

    for (int i = 0; i < 3; i++) {
        lv_obj_t * p = ui_group_create(pages);
        lv_obj_set_size(p, LV_PCT(100), 376);
        lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(p, TF_GAP, 0);
        s_pages[i] = p;
    }
    build_page_monitor(s_pages[0]);
    build_page_trend(s_pages[1]);
    build_page_settings(s_pages[2]);

    build_nav(s_root);
    ui_tuf_set_page(0);
    return s_root;
}

void ui_tuf_set_page(int page)
{
    if (page < 0 || page > 2) return;
    s_page = page;
    for (int i = 0; i < 3; i++) {
        if (i == page) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        bool active = (i == page);
        lv_obj_set_style_bg_opa(s_nav_btn[i], active ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_nav_btn[i], active ? 2 : 0, 0);
        col(s_nav_lbl[i], active ? TF_AMBER : TF_DIM);
    }
}

int ui_tuf_page_count(void)
{
    return 3;
}

/* ============================== 更新 ============================== */

static float dec_scale(const dash_history_t * h)
{
    float mx = hist_max(h, h->tps_dec);
    return mx < 10.0f ? 10.0f : mx * 1.2f;
}

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

static void set_dec_segments(float pct)
{
    int on = (int)(pct / 10.0f + 0.5f);
    if (on < 0) on = 0;
    if (on > 10) on = 10;
    if (on == s_seg_last) return;
    s_seg_last = on;
    for (int i = 0; i < 10; i++) {
        lv_obj_set_style_bg_color(s_seg[i],
            lv_color_hex(i < on ? (i < 7 ? TF_AMBER : TF_RED) : TF_BORDER), 0);
        lv_obj_set_style_bg_opa(s_seg[i], LV_OPA_COVER, 0);
    }
}

void ui_tuf_apply(const dash_metrics_t * m, const dash_history_t * h,
                  bool new_sample)
{
    char b[64];
    int pct;

    txt(s_rot_lbl, dashboard_ui_rot180() ? "180" : "0");
    txt(s_model, m->model[0] ? m->model : "WAITING");
    if (!m->online) set_status(TF_RED, m->stale ? "重连" : "离线");
    else if (m->busy) set_status(TF_AMBER, "解码");
    else set_status(TF_LIME, "在线");

    if (m->has_tps && m->online) {
        fmt_num(b, sizeof(b), m->tps_dec, m->tps_dec >= 100 ? 0 : 1);
        txt(s_dec_now, b);
        col(s_dec_now, TF_TEXT);
        txt(s_dec_mode, m->tps_live ? "LIVE" : "LAST");
        col(s_dec_mode, m->tps_live ? TF_LIME : TF_AMBER);
        fmt_num(b, sizeof(b), m->tps_avg, 1);
        txt(s_dec_avg, b);
        fmt_num(b, sizeof(b), m->tps_peak, 1);
        txt(s_dec_peak, b);
        set_dec_segments(dec_scale(h) > 0 ? m->tps_dec * 100.0f / dec_scale(h) : 0);
    } else {
        txt(s_dec_now, "--");
        col(s_dec_now, TF_MUT);
        txt(s_dec_mode, m->online ? "NODATA" : "OFFLINE");
        col(s_dec_mode, TF_MUT);
        txt(s_dec_avg, "--");
        txt(s_dec_peak, "--");
        set_dec_segments(0);
    }

    if (m->has_tps && m->tps_peak_pre > 0) {
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
        col(s_pwr, m->power_w > 200 ? TF_RED : TF_AMBER);
    } else { txt(s_pwr, "--"); col(s_pwr, TF_MUT); }

    if (m->has_temp) {
        snprintf(b, sizeof(b), "%.0fC", (double)m->temp_c);
        txt(s_tmp, b);
        col(s_tmp, m->temp_c >= 80 ? TF_RED : TF_TEXT);
    } else { txt(s_tmp, "--"); col(s_tmp, TF_MUT); }

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

    /* 趋势页 */
    if (h->count > 0) {
        fmt_num(b, sizeof(b), h->tps_dec[h->count - 1], 1);
        txt(s_trend_now, b);
        fmt_num(b, sizeof(b), hist_avg(h, h->tps_dec), 1);
        txt(s_trend_avg, b);
        fmt_num(b, sizeof(b), hist_max(h, h->tps_dec), 1);
        txt(s_trend_peak, b);

        float pnow = h->tps_pre[h->count - 1];
        float pmax = hist_max(h, h->tps_pre);
        snprintf(b, sizeof(b), "%.0f / %.0f", (double)pnow, (double)pmax);
        txt(s_trend_pre, b);
    }
    if (m->has_requests) {
        fmt_count(b, sizeof(b), m->requests_total);
        txt(s_stat_req, b);
    } else { txt(s_stat_req, "--"); }

    /* 设置页 */
    txt(s_set_provider, m->provider_name[0] ? m->provider_name : "自动识别");
    txt(s_set_endpoint, m->endpoint[0] ? m->endpoint : "未设置");
    txt(s_set_key, m->api_key_mask[0] ? m->api_key_mask : "未设置");
    txt(s_about_model, m->model[0] ? m->model : "未知");
    if (m->has_host_uptime) {
        snprintf(b, sizeof(b), "%ud %uh",
                 (unsigned)(m->host_uptime_s / 86400),
                 (unsigned)((m->host_uptime_s % 86400) / 3600));
        txt(s_about_up, b);
    } else {
        txt(s_about_up, "--");
    }

    /* 图表 */
    if (new_sample && h->count > 0) {
        int32_t d = (int32_t)(h->tps_dec[h->count - 1] * 10.0f + 0.5f);
        int32_t p = (int32_t)(h->tps_pre[h->count - 1] * 10.0f + 0.5f);
        lv_chart_set_next_value(s_chart, s_ser1, d);
        lv_chart_set_next_value(s_chart, s_ser2, p);
        lv_chart_set_next_value(s_trend_chart, s_trend1, d);
        lv_chart_set_next_value(s_trend_chart, s_trend2, p);

        int32_t dmax = (int32_t)(dec_scale(h) * 10.0f + 0.5f);
        float pmx = hist_max(h, h->tps_pre);
        int32_t pmax = (int32_t)((pmx < 100.0f ? 100.0f : pmx * 1.2f) * 10.0f + 0.5f);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, dmax);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, pmax);
        lv_chart_set_range(s_trend_chart, LV_CHART_AXIS_PRIMARY_Y, 0, dmax);
        lv_chart_set_range(s_trend_chart, LV_CHART_AXIS_SECONDARY_Y, 0, pmax);
        lv_chart_refresh(s_chart);
        lv_chart_refresh(s_trend_chart);
    }
}
