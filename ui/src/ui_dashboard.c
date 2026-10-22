/**
 * 320×480 LLM 服务监控看板 —— 4 页：概览 / 曲线 / 系统 / 设置
 *
 * 纵向预算（页面高 334）：
 *   概览  hero 124 + 8 + 双行卡片 156 + 8 + 用量条 36 = 332
 *   曲线  分段 32 + 8 + 图表卡 206 + 8 + 统计 72 = 326
 *   系统  双行卡片 148 + 8 + 时间 72 + 8 + 缓存 68 + 6 + 设备行 18 = 328
 *   设置  网络 74 + 8 + API 86 + 8 + 按钮 40 + 8 + 按钮 40 + 8 + 版本 62 = 334
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef DASH_FW_VERSION
#define DASH_FW_VERSION "sim"
#endif

#define PAGE_W 320
#define PAGE_H 334
#define PAGE_PAD 8
#define PAGE_GAP 6
#define CONTENT_H 334

/* ============================== 状态 ============================== */

static dash_metrics_t s_m;
static dash_history_t s_h;
static int s_page = 0;
static dash_action_cb_t s_action_cb;

static lv_obj_t * s_pages[4];
static lv_obj_t * s_nav_btn[4];
static lv_obj_t * s_nav_lbl[4];

static lv_obj_t * s_wifi_lbl;
static char s_clock_buf[8] = "--:--";
static lv_obj_t * s_model_lbl;
static lv_obj_t * s_sub_lbl;
static lv_obj_t * s_status_box;
static lv_obj_t * s_status_lbl;
static lv_obj_t * s_status_dot;

/* 概览 */
static lv_obj_t * s_hero_cap;
static lv_obj_t * s_hero_mode;
static lv_obj_t * s_hero_val;
static lv_obj_t * s_hero_unit;
static lv_obj_t * s_hero_avg;
static lv_obj_t * s_hero_peak;
static lv_obj_t * s_hero_chart;
static lv_chart_series_t * s_hero_ser;
static lv_obj_t * s_con_val;
static lv_obj_t * s_con_bar;
static lv_obj_t * s_queue_val;
static lv_obj_t * s_queue_bar;
static lv_obj_t * s_kv_val;
static lv_obj_t * s_kv_bar;
static lv_obj_t * s_mtp_val;
static lv_obj_t * s_mtp_bar;
static lv_obj_t * s_usage_lbl;

/* 曲线 */
static lv_obj_t * s_seg_btn[4];
static lv_obj_t * s_seg_lbl[4];
static int s_chart_kind = 0;
static lv_obj_t * s_chart_title;
static lv_obj_t * s_chart_scale;
static lv_obj_t * s_chart_box[4];
static lv_obj_t * s_chart[4];
static lv_chart_series_t * s_chart_ser1[4];
static lv_chart_series_t * s_chart_ser2[4];
static lv_obj_t * s_legend1;
static lv_obj_t * s_legend2;
static lv_obj_t * s_stat_cap[4];
static lv_obj_t * s_stat_val[4];

/* 系统 */
static lv_obj_t * s_sys_power;
static lv_obj_t * s_sys_temp;
static lv_obj_t * s_sys_gpu;
static lv_obj_t * s_sys_ram;
static lv_obj_t * s_sys_power_sub;
static lv_obj_t * s_sys_temp_sub;
static lv_obj_t * s_sys_gpu_sub;
static lv_obj_t * s_sys_ram_sub;
static lv_obj_t * s_up_host;
static lv_obj_t * s_up_model;
static lv_obj_t * s_up_engine;
static lv_obj_t * s_up_device;
static lv_obj_t * s_cache_hit;
static lv_obj_t * s_cache_size;
static lv_obj_t * s_cache_entries;
static lv_obj_t * s_cache_disk;
static lv_obj_t * s_sys_device;

/* 设置 */
static lv_obj_t * s_set_ssid;
static lv_obj_t * s_set_ip;
static lv_obj_t * s_set_rssi;
static lv_obj_t * s_set_net_state;
static lv_obj_t * s_set_provider;
static lv_obj_t * s_set_endpoint;
static lv_obj_t * s_set_key;
static lv_obj_t * s_set_host;
static lv_obj_t * s_set_version;

/* 配网浮层 */
static lv_obj_t * s_overlay;
static lv_obj_t * s_prov_ssid;
static lv_obj_t * s_prov_pass;
static lv_obj_t * s_prov_url;
static lv_obj_t * s_prov_state;

/* Toast */
static lv_obj_t * s_toast;
static lv_timer_t * s_toast_timer;

/* ============================== 小工具 ============================== */

static void label_text(lv_obj_t * l, const char * text)
{
    if (l && text) lv_label_set_text(l, text);
}

static void label_color(lv_obj_t * l, uint32_t color)
{
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

static void update_top_right(void)
{
    char buf[48];
    if (s_m.wifi_rssi != 0) {
        snprintf(buf, sizeof(buf), "Wi-Fi %d dBm  |  %s",
                 (int)s_m.wifi_rssi, s_clock_buf);
    } else {
        snprintf(buf, sizeof(buf), "Wi-Fi --  |  %s", s_clock_buf);
    }
    label_text(s_wifi_lbl, buf);
}

static void fmt_float(char * buf, size_t n, float v, int prec)
{
    snprintf(buf, n, "%.*f", prec, (double)v);
}

static void fmt_count(char * buf, size_t n, uint64_t v)
{
    if (v >= 1000000000ULL) {
        snprintf(buf, n, "%.2fG", (double)v / 1e9);
    } else if (v >= 1000000ULL) {
        snprintf(buf, n, "%.2fM", (double)v / 1e6);
    } else if (v >= 1000ULL) {
        snprintf(buf, n, "%.1fk", (double)v / 1e3);
    } else {
        snprintf(buf, n, "%llu", (unsigned long long)v);
    }
}

static void fmt_bytes(char * buf, size_t n, uint64_t v)
{
    if (v >= (1ULL << 30)) {
        snprintf(buf, n, "%.2fG", (double)v / (double)(1ULL << 30));
    } else if (v >= (1ULL << 20)) {
        snprintf(buf, n, "%.0fM", (double)v / (double)(1ULL << 20));
    } else {
        snprintf(buf, n, "%.0fK", (double)v / 1024.0);
    }
}

static void fmt_uptime(char * buf, size_t n, uint32_t sec)
{
    if (sec >= 86400U) {
        snprintf(buf, n, "%ud %uh", (unsigned)(sec / 86400U),
                 (unsigned)((sec % 86400U) / 3600U));
    } else if (sec >= 3600U) {
        snprintf(buf, n, "%uh %um", (unsigned)(sec / 3600U),
                 (unsigned)((sec % 3600U) / 60U));
    } else if (sec >= 60U) {
        snprintf(buf, n, "%um", (unsigned)(sec / 60U));
    } else {
        snprintf(buf, n, "%us", (unsigned)sec);
    }
}

static float history_max(const float * a)
{
    float m = 0.0f;
    for (int i = 0; i < s_h.count; i++) {
        if (a[i] > m) m = a[i];
    }
    return m;
}

static float history_avg(const float * a)
{
    if (s_h.count == 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < s_h.count; i++) sum += a[i];
    return sum / (float)s_h.count;
}

static float history_min_nonzero(const float * a)
{
    float m = 1e9f;
    for (int i = 0; i < s_h.count; i++) {
        if (a[i] < m) m = a[i];
    }
    return (m == 1e9f) ? 0.0f : m;
}

static void history_push(void)
{
    if (s_h.count < DASH_HISTORY_LEN) {
        int i = s_h.count;
        s_h.tps_dec[i] = s_m.has_tps ? s_m.tps_dec : 0.0f;
        s_h.tps_pre[i] = s_m.has_tps ? s_m.tps_pre : 0.0f;
        s_h.kv[i] = s_m.has_kv ? s_m.kv_pct : 0.0f;
        s_h.conc[i] = s_m.has_concurrency ? (float)s_m.in_flight : 0.0f;
        s_h.power[i] = s_m.has_power ? s_m.power_w : 0.0f;
        s_h.temp[i] = s_m.has_temp ? s_m.temp_c : 0.0f;
        s_h.count++;
    } else {
        memmove(&s_h.tps_dec[0], &s_h.tps_dec[1], sizeof(float) * (DASH_HISTORY_LEN - 1));
        memmove(&s_h.tps_pre[0], &s_h.tps_pre[1], sizeof(float) * (DASH_HISTORY_LEN - 1));
        memmove(&s_h.kv[0], &s_h.kv[1], sizeof(float) * (DASH_HISTORY_LEN - 1));
        memmove(&s_h.conc[0], &s_h.conc[1], sizeof(float) * (DASH_HISTORY_LEN - 1));
        memmove(&s_h.power[0], &s_h.power[1], sizeof(float) * (DASH_HISTORY_LEN - 1));
        memmove(&s_h.temp[0], &s_h.temp[1], sizeof(float) * (DASH_HISTORY_LEN - 1));
        int i = DASH_HISTORY_LEN - 1;
        s_h.tps_dec[i] = s_m.has_tps ? s_m.tps_dec : 0.0f;
        s_h.tps_pre[i] = s_m.has_tps ? s_m.tps_pre : 0.0f;
        s_h.kv[i] = s_m.has_kv ? s_m.kv_pct : 0.0f;
        s_h.conc[i] = s_m.has_concurrency ? (float)s_m.in_flight : 0.0f;
        s_h.power[i] = s_m.has_power ? s_m.power_w : 0.0f;
        s_h.temp[i] = s_m.has_temp ? s_m.temp_c : 0.0f;
    }
}

static void set_status(uint32_t color, const char * text)
{
    if (s_status_box) {
        lv_obj_set_style_bg_color(s_status_box, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(s_status_box, LV_OPA_20, 0);
    }
    label_color(s_status_lbl, color);
    if (s_status_dot) {
        lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(color), 0);
    }
    label_text(s_status_lbl, text);
}

static void set_seg_active(void)
{
    for (int i = 0; i < 4; i++) {
        bool active = (i == s_chart_kind);
        lv_obj_set_style_bg_opa(s_seg_btn[i], active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_seg_btn[i], active ? 1 : 0, 0);
        label_color(s_seg_lbl[i], active ? UI_ACCENT : UI_TEXT_DIM);
    }
}

static void set_nav_active(void)
{
    for (int i = 0; i < 4; i++) {
        bool active = (i == s_page);
        lv_obj_set_style_bg_opa(s_nav_btn[i], active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_nav_btn[i], active ? 1 : 0, 0);
        label_color(s_nav_lbl[i], active ? UI_ACCENT : UI_TEXT_DIM);
    }
}

/* ============================== 共享头 ============================== */

static void build_status_bar(lv_obj_t * parent)
{
    lv_obj_t * bar = ui_group_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 20);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * l = ui_cjk_create(bar, "LLM 服务监控");
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    label_color(l, UI_TEXT_DIM);

    lv_obj_t * right = lv_label_create(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(right, UI_F_CAP, 0);
    label_color(right, UI_TEXT_MUT);
    lv_label_set_text(right, "Wi-Fi --  ·  --:--");
    s_wifi_lbl = right;
}

static void build_header(lv_obj_t * parent)
{
    lv_obj_t * row = ui_group_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 42);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * col = ui_group_create(row);
    lv_obj_set_size(col, 214, 42);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_model_lbl = lv_label_create(col);
    lv_obj_set_size(s_model_lbl, 214, 20);
    lv_label_set_long_mode(s_model_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_model_lbl, UI_F_CJK, 0);
    label_color(s_model_lbl, UI_TEXT);
    lv_label_set_text(s_model_lbl, "等待连接");

    s_sub_lbl = lv_label_create(col);
    lv_obj_set_size(s_sub_lbl, 214, 22);
    lv_label_set_long_mode(s_sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_sub_lbl, UI_F_CJK, 0);
    label_color(s_sub_lbl, UI_TEXT_MUT);
    lv_label_set_text(s_sub_lbl, "未配置服务器");

    s_status_box = ui_group_create(row);
    lv_obj_set_size(s_status_box, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_color(s_status_box, lv_color_hex(UI_TEXT_MUT), 0);
    lv_obj_set_style_bg_opa(s_status_box, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_status_box, UI_R_PILL, 0);
    lv_obj_set_style_pad_hor(s_status_box, 8, 0);
    lv_obj_set_flex_flow(s_status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_status_box, 4, 0);

    s_status_dot = ui_group_create(s_status_box);
    lv_obj_set_size(s_status_dot, 6, 6);
    lv_obj_set_style_radius(s_status_dot, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(UI_TEXT_MUT), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);

    s_status_lbl = lv_label_create(s_status_box);
    lv_obj_set_style_text_font(s_status_lbl, UI_F_CJK, 0);
    label_color(s_status_lbl, UI_TEXT_MUT);
    lv_label_set_text(s_status_lbl, "离线");
}

/* ============================== 概览 ============================== */

static lv_obj_t * build_stat_card(lv_obj_t * parent, const char * cap,
                                  lv_obj_t ** value, lv_obj_t ** bar,
                                  uint32_t color, int32_t bar_max)
{
    lv_obj_t * card = ui_card_create(parent);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 1, 0);

    ui_caption_create(card, cap);

    lv_obj_t * v = lv_label_create(card);
    lv_obj_set_style_text_font(v, UI_F_H2, 0);
    label_color(v, UI_TEXT);
    lv_label_set_text(v, "--");
    *value = v;

    if (bar) {
        lv_obj_t * b = ui_bar_create(card, bar_max, color);
        *bar = b;
    } else {
        *bar = NULL;
    }
    return card;
}

static void build_overview(lv_obj_t * page)
{
    /* Hero */
    lv_obj_t * hero = ui_card_create(page);
    lv_obj_set_size(hero, LV_PCT(100), 124);
    lv_obj_set_style_pad_all(hero, 8, 0);
    lv_obj_set_style_pad_row(hero, 0, 0);

    lv_obj_t * top = ui_group_create(hero);
    lv_obj_set_size(top, LV_PCT(100), 18);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_hero_cap = ui_caption_create(top, "解码速度");
    label_color(s_hero_cap, UI_TEXT_DIM);

    lv_obj_t * mode = ui_group_create(top);
    lv_obj_set_size(mode, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(mode, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(mode, LV_OPA_20, 0);
    lv_obj_set_style_radius(mode, UI_R_PILL, 0);
    lv_obj_set_style_pad_hor(mode, 6, 0);
    lv_obj_set_style_pad_ver(mode, 1, 0);
    s_hero_mode = lv_label_create(mode);
    lv_obj_set_style_text_font(s_hero_mode, UI_F_CJK, 0);
    label_color(s_hero_mode, UI_ACCENT);
    lv_label_set_text(s_hero_mode, "等待");

    lv_obj_t * valrow = ui_group_create(hero);
    lv_obj_set_size(valrow, LV_PCT(100), 49);
    lv_obj_set_flex_flow(valrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(valrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(valrow, 5, 0);

    s_hero_val = lv_label_create(valrow);
    lv_obj_set_style_text_font(s_hero_val, &lv_font_montserrat_40, 0);
    label_color(s_hero_val, UI_TEXT);
    lv_label_set_text(s_hero_val, "--");

    s_hero_unit = lv_label_create(valrow);
    lv_obj_set_style_text_font(s_hero_unit, UI_F_CAP, 0);
    label_color(s_hero_unit, UI_TEXT_MUT);
    lv_label_set_text(s_hero_unit, "tok/s");

    lv_obj_t * sub = ui_group_create(hero);
    lv_obj_set_size(sub, LV_PCT(100), 19);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(sub, 12, 0);

    lv_obj_t * a = ui_cjk_create(sub, "均速");
    label_color(a, UI_TEXT_MUT);
    s_hero_avg = lv_label_create(sub);
    lv_obj_set_style_text_font(s_hero_avg, UI_F_BODY, 0);
    label_color(s_hero_avg, UI_TEXT_DIM);
    lv_label_set_text(s_hero_avg, "--");

    lv_obj_t * b = ui_cjk_create(sub, "峰值");
    label_color(b, UI_TEXT_MUT);
    s_hero_peak = lv_label_create(sub);
    lv_obj_set_style_text_font(s_hero_peak, UI_F_BODY, 0);
    label_color(s_hero_peak, UI_ACCENT_2);
    lv_label_set_text(s_hero_peak, "--");

    s_hero_chart = ui_chart_create(hero, 18, UI_ACCENT, 0,
                                   &s_hero_ser, NULL);
    lv_chart_set_div_line_count(s_hero_chart, 0, 0);
    lv_chart_set_range(s_hero_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

    /* 2×2 指标卡 */
    lv_obj_t * grid = ui_group_create(page);
    lv_obj_set_size(grid, LV_PCT(100), 156);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, 8, 0);

    lv_obj_t * r1 = ui_group_create(grid);
    lv_obj_set_size(r1, LV_PCT(100), 74);
    lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r1, 8, 0);
    lv_obj_t * c1 = build_stat_card(r1, "并发", &s_con_val, &s_con_bar,
                                    UI_ACCENT, 3);
    lv_obj_set_size(c1, LV_PCT(50) - 4, 74);
    lv_obj_t * c2 = build_stat_card(r1, "排队", &s_queue_val, &s_queue_bar,
                                    UI_WARN, 10);
    lv_obj_set_size(c2, LV_PCT(50) - 4, 74);

    lv_obj_t * r2 = ui_group_create(grid);
    lv_obj_set_size(r2, LV_PCT(100), 74);
    lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r2, 8, 0);
    lv_obj_t * c3 = build_stat_card(r2, "KV 占用", &s_kv_val, &s_kv_bar,
                                    UI_CYAN, 100);
    lv_obj_set_size(c3, LV_PCT(50) - 4, 74);
    lv_obj_t * c4 = build_stat_card(r2, "MTP 接受", &s_mtp_val, &s_mtp_bar,
                                    UI_OK, 100);
    lv_obj_set_size(c4, LV_PCT(50) - 4, 74);

    /* 用量条 */
    lv_obj_t * usage = ui_card_create(page);
    lv_obj_set_size(usage, LV_PCT(100), 38);
    lv_obj_set_style_pad_all(usage, 8, 0);
    lv_obj_set_style_pad_row(usage, 0, 0);
    lv_obj_set_flex_align(usage, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_usage_lbl = ui_cjk_create(usage, "累计 --  ·  缓存 --  ·  请求 --");
    lv_obj_set_size(s_usage_lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    label_color(s_usage_lbl, UI_TEXT_DIM);
}

/* ============================== 曲线 ============================== */

static void select_chart(int kind);

static void seg_cb(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    select_chart(idx);
}

static lv_obj_t * build_seg(lv_obj_t * parent, const char * text, int idx)
{
    lv_obj_t * b = ui_group_create(parent);
    lv_obj_set_size(b, LV_PCT(25) - 3, 30);
    lv_obj_set_style_radius(b, UI_R_S, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    lv_label_set_text(l, text);
    label_color(l, UI_TEXT_DIM);

    s_seg_btn[idx] = b;
    s_seg_lbl[idx] = l;
    return b;
}

static void build_legend(lv_obj_t * parent, const char * text, uint32_t color,
                         lv_obj_t ** out)
{
    lv_obj_t * g = ui_group_create(parent);
    lv_obj_set_size(g, LV_SIZE_CONTENT, 20);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(g, 4, 0);

    lv_obj_t * dot = ui_group_create(g);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, UI_R_PILL, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t * l = lv_label_create(g);
    lv_obj_set_size(l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    label_color(l, UI_TEXT_DIM);
    lv_label_set_text(l, text);
    *out = g;
}

static void build_charts(lv_obj_t * page)
{
    lv_obj_t * seg = ui_group_create(page);
    lv_obj_set_size(seg, LV_PCT(100), 32);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    build_seg(seg, "速度", 0);
    build_seg(seg, "KV", 1);
    build_seg(seg, "并发", 2);
    build_seg(seg, "功耗", 3);

    lv_obj_t * card = ui_card_create(page);
    lv_obj_set_size(card, LV_PCT(100), 214);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 1, 0);

    lv_obj_t * top = ui_group_create(card);
    lv_obj_set_size(top, LV_PCT(100), 20);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_chart_title = ui_cjk_create(top, "速度 (tok/s)");
    label_color(s_chart_title, UI_TEXT);

    s_chart_scale = lv_label_create(top);
    lv_obj_set_size(s_chart_scale, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_chart_scale, UI_F_CJK, 0);
    label_color(s_chart_scale, UI_TEXT_MUT);
    lv_label_set_text(s_chart_scale, "0 - 100");

    /* 4 张图叠放，切页时只显示一张 */
    lv_obj_t * stack = ui_group_create(card);
    lv_obj_set_size(stack, LV_PCT(100), 154);
    lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t * box = ui_group_create(stack);
        lv_obj_set_size(box, LV_PCT(100), 154);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 0);
        s_chart_box[i] = box;
        uint32_t c1 = (i == 0) ? UI_ACCENT : (i == 1 ? UI_CYAN :
                       (i == 2 ? UI_ACCENT_2 : UI_WARN));
        uint32_t c2 = (i == 0) ? UI_ACCENT_2 : 0;
        s_chart[i] = ui_chart_create(box, 154, c1, c2,
                                     &s_chart_ser1[i], &s_chart_ser2[i]);
    }

    lv_obj_t * legend = ui_group_create(card);
    lv_obj_set_size(legend, LV_PCT(100), 20);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 10, 0);
    build_legend(legend, "解码", UI_ACCENT, &s_legend1);
    build_legend(legend, "预填充", UI_ACCENT_2, &s_legend2);

    /* 统计四宫格 */
    lv_obj_t * stats = ui_group_create(page);
    lv_obj_set_size(stats, LV_PCT(100), 72);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stats, 6, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t * c = ui_card_create(stats);
        lv_obj_set_size(c, LV_PCT(25) - 5, 72);
        lv_obj_set_style_pad_all(c, 7, 0);
        lv_obj_set_style_pad_row(c, 1, 0);
        s_stat_cap[i] = ui_caption_create(c, "--");
        s_stat_val[i] = lv_label_create(c);
        lv_obj_set_style_text_font(s_stat_val[i], UI_F_H3, 0);
        label_color(s_stat_val[i], UI_TEXT);
        lv_label_set_text(s_stat_val[i], "--");
    }

    select_chart(0);
}

/* ============================== 系统 ============================== */

static lv_obj_t * build_sys_card(lv_obj_t * parent, const char * cap,
                                 const char * unit, lv_obj_t ** value,
                                 lv_obj_t ** sub, uint32_t color)
{
    lv_obj_t * card = ui_card_create(parent);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 1, 0);

    /* 标题行：左侧指标名，右侧数据来源提示 */
    lv_obj_t * top = ui_group_create(card);
    lv_obj_set_size(top, LV_PCT(100), 22);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * c = ui_caption_create(top, cap);
    label_color(c, UI_TEXT_DIM);
    lv_obj_t * s = ui_caption_create(top, "");
    lv_obj_set_style_text_font(s, UI_F_CJK, 0);
    label_color(s, UI_TEXT_MUT);
    *sub = s;

    /* 数值行：24 号字，行高 26 足以容纳 */
    lv_obj_t * row = ui_group_create(card);
    lv_obj_set_size(row, LV_PCT(100), 26);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 3, 0);

    lv_obj_t * v = lv_label_create(row);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_24, 0);
    label_color(v, color);
    lv_label_set_text(v, "--");
    *value = v;

    lv_obj_t * u = lv_label_create(row);
    lv_obj_set_style_text_font(u, UI_F_CAP, 0);
    label_color(u, UI_TEXT_MUT);
    lv_label_set_text(u, unit);
    return card;
}

static void build_uptime_cell(lv_obj_t * parent, const char * cap, lv_obj_t ** value)
{
    lv_obj_t * col = ui_group_create(parent);
    lv_obj_set_size(col, LV_PCT(25), 52);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t * c = ui_caption_create(col, cap);
    label_color(c, UI_TEXT_MUT);
    lv_obj_t * v = lv_label_create(col);
    lv_obj_set_style_text_font(v, UI_F_BODY, 0);
    label_color(v, UI_TEXT);
    lv_label_set_text(v, "--");
    *value = v;
}

static void build_system(lv_obj_t * page)
{
    lv_obj_t * r1 = ui_group_create(page);
    lv_obj_set_size(r1, LV_PCT(100), 70);
    lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r1, 8, 0);
    lv_obj_t * p = build_sys_card(r1, "功耗", "W", &s_sys_power, &s_sys_power_sub, UI_WARN);
    lv_obj_set_size(p, LV_PCT(50) - 4, 70);
    lv_obj_t * t = build_sys_card(r1, "温度", "C", &s_sys_temp, &s_sys_temp_sub, UI_ERR);
    lv_obj_set_size(t, LV_PCT(50) - 4, 70);

    lv_obj_t * r2 = ui_group_create(page);
    lv_obj_set_size(r2, LV_PCT(100), 70);
    lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r2, 8, 0);
    lv_obj_t * g = build_sys_card(r2, "GPU", "%", &s_sys_gpu, &s_sys_gpu_sub, UI_OK);
    lv_obj_set_size(g, LV_PCT(50) - 4, 70);
    lv_obj_t * m = build_sys_card(r2, "内存", "%", &s_sys_ram, &s_sys_ram_sub, UI_CYAN);
    lv_obj_set_size(m, LV_PCT(50) - 4, 70);

    lv_obj_t * up = ui_card_create(page);
    lv_obj_set_size(up, LV_PCT(100), 72);
    lv_obj_set_style_pad_all(up, 8, 0);
    lv_obj_set_style_pad_row(up, 0, 0);
    lv_obj_set_flex_flow(up, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(up, 0, 0);
    build_uptime_cell(up, "主机开机", &s_up_host);
    build_uptime_cell(up, "模型运行", &s_up_model);
    build_uptime_cell(up, "引擎累计", &s_up_engine);
    build_uptime_cell(up, "设备运行", &s_up_device);

    lv_obj_t * cache = ui_card_create(page);
    lv_obj_set_size(cache, LV_PCT(100), 68);
    lv_obj_set_style_pad_all(cache, 8, 0);
    lv_obj_set_style_pad_row(cache, 0, 0);

    lv_obj_t * c1 = ui_group_create(cache);
    lv_obj_set_size(c1, LV_PCT(100), 19);
    lv_obj_set_flex_flow(c1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c1, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * cl = ui_caption_create(c1, "提示缓存");
    label_color(cl, UI_TEXT_DIM);
    s_cache_hit = lv_label_create(c1);
    lv_obj_set_size(s_cache_hit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_cache_hit, UI_F_CJK, 0);
    label_color(s_cache_hit, UI_OK);
    lv_label_set_text(s_cache_hit, "命中 --");

    lv_obj_t * c2 = ui_group_create(cache);
    lv_obj_set_size(c2, LV_PCT(100), 19);
    lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(c2, 12, 0);
    lv_obj_set_flex_align(c2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    s_cache_size = lv_label_create(c2);
    lv_obj_set_size(s_cache_size, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_cache_size, UI_F_CJK, 0);
    label_color(s_cache_size, UI_TEXT_MUT);
    lv_label_set_text(s_cache_size, "容量 --");
    s_cache_entries = lv_label_create(c2);
    lv_obj_set_size(s_cache_entries, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_cache_entries, UI_F_CJK, 0);
    label_color(s_cache_entries, UI_TEXT_MUT);
    lv_label_set_text(s_cache_entries, "条目 --");
    s_cache_disk = lv_label_create(c2);
    lv_obj_set_size(s_cache_disk, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_cache_disk, UI_F_CJK, 0);
    label_color(s_cache_disk, UI_TEXT_MUT);
    lv_label_set_text(s_cache_disk, "用量 --");

    s_sys_device = ui_cjk_create(page, "");
    lv_obj_set_size(s_sys_device, LV_PCT(100), 18);
    label_color(s_sys_device, UI_TEXT_MUT);
}

/* ============================== 设置 ============================== */

static void action_event_cb(lv_event_t * e)
{
    dash_action_t a = (dash_action_t)(intptr_t)lv_event_get_user_data(e);
    if (s_action_cb) s_action_cb(a);
}

static lv_obj_t * build_action_btn(lv_obj_t * parent, const char * text,
                                   uint32_t color, dash_action_t action)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_radius(b, UI_R_M, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)action);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UI_F_CJK, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return b;
}

static lv_obj_t * build_kv_row(lv_obj_t * parent, const char * key,
                               lv_obj_t ** value, uint32_t color)
{
    lv_obj_t * row = ui_group_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 21);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * k = ui_cjk_create(row, key);
    label_color(k, UI_TEXT_MUT);
    lv_obj_t * v = lv_label_create(row);
    lv_obj_set_size(v, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(v, UI_F_CJK, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(color), 0);
    lv_label_set_text(v, "--");
    if (value) *value = v;
    return row;
}

static void build_settings(lv_obj_t * page)
{
    lv_obj_t * net = ui_card_create(page);
    lv_obj_set_size(net, LV_PCT(100), 100);
    lv_obj_set_style_pad_all(net, 8, 0);
    lv_obj_set_style_pad_row(net, 0, 0);
    build_kv_row(net, "Wi-Fi", &s_set_ssid, UI_TEXT);
    build_kv_row(net, "IP 地址", &s_set_ip, UI_TEXT_DIM);
    build_kv_row(net, "信号", &s_set_rssi, UI_TEXT_DIM);
    s_set_net_state = ui_cjk_create(net, "未连接");
    lv_obj_set_size(s_set_net_state, LV_PCT(100), 21);
    lv_obj_set_style_text_font(s_set_net_state, UI_F_CJK, 0);
    label_color(s_set_net_state, UI_TEXT_MUT);

    lv_obj_t * api = ui_card_create(page);
    lv_obj_set_size(api, LV_PCT(100), 100);
    lv_obj_set_style_pad_all(api, 8, 0);
    lv_obj_set_style_pad_row(api, 0, 0);
    build_kv_row(api, "服务端", &s_set_provider, UI_ACCENT);
    build_kv_row(api, "地址", &s_set_endpoint, UI_TEXT_DIM);
    build_kv_row(api, "密钥", &s_set_key, UI_TEXT_MUT);
    build_kv_row(api, "主机监控", &s_set_host, UI_TEXT_MUT);

    lv_obj_t * row1 = ui_group_create(page);
    lv_obj_set_size(row1, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row1, 8, 0);
    lv_obj_t * b1 = build_action_btn(row1, "重新配网", UI_ACCENT, DASH_ACTION_PROVISION);
    lv_obj_set_size(b1, LV_PCT(50) - 4, 40);
    lv_obj_t * b2 = build_action_btn(row1, "立即刷新", UI_CYAN, DASH_ACTION_REFRESH);
    lv_obj_set_size(b2, LV_PCT(50) - 4, 40);

    lv_obj_t * row2 = ui_group_create(page);
    lv_obj_set_size(row2, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row2, 8, 0);
    lv_obj_t * b3 = build_action_btn(row2, "重启设备", UI_ERR, DASH_ACTION_REBOOT);
    lv_obj_set_size(b3, LV_PCT(50) - 4, 40);
    lv_obj_t * b4 = build_action_btn(row2, "清空峰值", UI_WARN, DASH_ACTION_CLEAR_PEAK);
    lv_obj_set_size(b4, LV_PCT(50) - 4, 40);

    lv_obj_t * foot = ui_group_create(page);
    lv_obj_set_size(foot, LV_PCT(100), 22);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * fl = ui_group_create(foot);
    lv_obj_set_size(fl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fl, 6, 0);
    lv_obj_t * fk = ui_caption_create(fl, "固件");
    label_color(fk, UI_TEXT_MUT);
    s_set_version = lv_label_create(fl);
    lv_obj_set_size(s_set_version, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_set_version, UI_F_CJK, 0);
    label_color(s_set_version, UI_TEXT_DIM);
    lv_label_set_text(s_set_version, DASH_FW_VERSION);

    lv_obj_t * hint = ui_caption_create(foot, "配网 192.168.4.1");
    label_color(hint, UI_TEXT_MUT);
}

/* ============================== 页面切换 ============================== */

static void nav_cb(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    dashboard_ui_set_page(idx);
}

void dashboard_ui_set_page(int page)
{
    if (page < 0 || page > 3) return;
    s_page = page;
    for (int i = 0; i < 4; i++) {
        if (i == page) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    set_nav_active();
}

int dashboard_ui_page(void)
{
    return s_page;
}

void dashboard_ui_set_action_cb(dash_action_cb_t cb)
{
    s_action_cb = cb;
}

/* ============================== 曲线数据 ============================== */

static void update_charts(void);

static void select_chart(int kind)
{
    if (kind < 0 || kind > 3) kind = 0;
    s_chart_kind = kind;
    for (int i = 0; i < 4; i++) {
        if (i == kind) lv_obj_remove_flag(s_chart_box[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_chart_box[i], LV_OBJ_FLAG_HIDDEN);
    }
    set_seg_active();

    const char * titles[4] = {
        "速度 (tok/s)",
        "KV 占用 (%)",
        "并发请求",
        "整机功耗 (W)",
    };
    label_text(s_chart_title, titles[kind]);

    if (kind == 0) {
        lv_obj_remove_flag(s_legend1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_legend2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_legend1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_legend2, LV_OBJ_FLAG_HIDDEN);
    }
    update_charts();
}

static void set_stat(int i, const char * cap, const char * val, uint32_t color)
{
    label_text(s_stat_cap[i], cap);
    label_text(s_stat_val[i], val);
    label_color(s_stat_val[i], color);
}

static float tps_scale(void)
{
    float mx = history_max(s_h.tps_dec);
    return (mx < 10.0f) ? 10.0f : mx * 1.2f;
}

static float conc_scale(void)
{
    float mx = history_max(s_h.conc);
    float s = (float)((s_m.slots > 2 ? s_m.slots : 2) + 1);
    return (mx + 1.0f > s) ? mx + 1.0f : s;
}

static float power_scale(void)
{
    float mx = history_max(s_h.power);
    return (mx < 50.0f) ? 50.0f : mx * 1.2f;
}

static float pre_scale(void)
{
    float mx = history_max(s_h.tps_pre);
    return (mx < 100.0f) ? 100.0f : mx * 1.2f;
}

static void update_charts(void)
{
    if (!s_chart[0]) return;

    const float * arr = s_h.tps_dec;
    if (s_chart_kind == 1) arr = s_h.kv;
    else if (s_chart_kind == 2) arr = s_h.conc;
    else if (s_chart_kind == 3) arr = s_h.power;

    char buf[32];
    if (s_chart_kind == 0) snprintf(buf, sizeof(buf), "解码 0-%.0f · 预填 0-%.0f",
                                    (double)tps_scale(), (double)pre_scale());
    else if (s_chart_kind == 1) snprintf(buf, sizeof(buf), "0 - 100%%");
    else if (s_chart_kind == 2) snprintf(buf, sizeof(buf), "0 - %.0f", (double)conc_scale());
    else snprintf(buf, sizeof(buf), "0 - %.0f W", (double)power_scale());
    label_text(s_chart_scale, buf);

    /* 统计 */
    if (s_h.count == 0) {
        for (int i = 0; i < 4; i++) set_stat(i, "--", "--", UI_TEXT_MUT);
        return;
    }

    float last = arr[s_h.count - 1];
    float avg = history_avg(arr);
    float hi = history_max(arr);
    float lo = history_min_nonzero(arr);

    if (s_chart_kind == 0) {
        snprintf(buf, sizeof(buf), "%.1f", (double)last);
        set_stat(0, "当前", buf, UI_ACCENT);
        snprintf(buf, sizeof(buf), "%.1f", (double)avg);
        set_stat(1, "平均", buf, UI_TEXT);
        snprintf(buf, sizeof(buf), "%.1f", (double)hi);
        set_stat(2, "峰值", buf, UI_ACCENT_2);
        snprintf(buf, sizeof(buf), "%.1f", (double)(s_h.tps_pre[s_h.count - 1]));
        set_stat(3, "预填充", buf, UI_CYAN);
    } else if (s_chart_kind == 1) {
        snprintf(buf, sizeof(buf), "%.1f%%", (double)last);
        set_stat(0, "当前", buf, UI_CYAN);
        snprintf(buf, sizeof(buf), "%.1f%%", (double)avg);
        set_stat(1, "平均", buf, UI_TEXT);
        snprintf(buf, sizeof(buf), "%.1f%%", (double)hi);
        set_stat(2, "最高", buf, UI_WARN);
        snprintf(buf, sizeof(buf), "%.1f%%", (double)lo);
        set_stat(3, "最低", buf, UI_OK);
    } else if (s_chart_kind == 2) {
        snprintf(buf, sizeof(buf), "%.0f", (double)last);
        set_stat(0, "当前", buf, UI_ACCENT_2);
        snprintf(buf, sizeof(buf), "%.0f", (double)hi);
        set_stat(1, "最高", buf, UI_WARN);
        snprintf(buf, sizeof(buf), "%u", (unsigned)s_m.queued);
        set_stat(2, "排队", buf, s_m.queued ? UI_ERR : UI_OK);
        snprintf(buf, sizeof(buf), "%u", (unsigned)s_m.slots);
        set_stat(3, "槽位", buf, UI_TEXT);
    } else {
        snprintf(buf, sizeof(buf), "%.0f", (double)last);
        set_stat(0, "当前", buf, UI_WARN);
        snprintf(buf, sizeof(buf), "%.0f", (double)avg);
        set_stat(1, "平均", buf, UI_TEXT);
        snprintf(buf, sizeof(buf), "%.0f", (double)hi);
        set_stat(2, "最高", buf, UI_ERR);
        snprintf(buf, sizeof(buf), "%.0f", (double)(s_m.has_temp ? s_m.temp_c : 0));
        set_stat(3, "温度", buf, UI_CYAN);
    }
}

/* ============================== 更新 ============================== */

static void update_header(void)
{
    char buf[128];
    if (s_m.model[0]) {
        label_text(s_model_lbl, s_m.model);
    } else {
        label_text(s_model_lbl, s_m.online ? "未知模型" : "等待连接");
    }

    if (s_m.provider_name[0]) {
        if (s_m.version[0]) {
            snprintf(buf, sizeof(buf), "%s  ·  v%s", s_m.provider_name, s_m.version);
        } else {
            snprintf(buf, sizeof(buf), "%s", s_m.provider_name);
        }
        label_text(s_sub_lbl, buf);
    } else {
        label_text(s_sub_lbl, "未配置服务器");
    }

    if (!s_m.online) {
        set_status(UI_ERR, s_m.stale ? "重连中" : "离线");
    } else if (s_m.busy) {
        set_status(UI_CYAN, "解码中");
    } else {
        set_status(UI_OK, "在线");
    }

    update_top_right();
}

static void update_overview(void)
{
    char buf[64];

    if (s_m.has_tps && s_m.online) {
        fmt_float(buf, sizeof(buf), s_m.tps_dec, s_m.tps_dec >= 100 ? 0 : 1);
        label_text(s_hero_val, buf);
        label_color(s_hero_val, UI_TEXT);
        label_text(s_hero_mode, s_m.tps_live ? "实时" : "最近");
        label_color(s_hero_mode, s_m.tps_live ? UI_OK : UI_WARN);

        snprintf(buf, sizeof(buf), "%.1f", (double)s_m.tps_avg);
        label_text(s_hero_avg, buf);
        snprintf(buf, sizeof(buf), "%.1f", (double)s_m.tps_peak);
        label_text(s_hero_peak, buf);
    } else {
        label_text(s_hero_val, "--");
        label_color(s_hero_val, UI_TEXT_MUT);
        label_text(s_hero_mode, s_m.online ? "无数据" : "离线");
        label_color(s_hero_mode, UI_TEXT_MUT);
        label_text(s_hero_avg, "--");
        label_text(s_hero_peak, "--");
    }

    if (s_m.has_concurrency) {
        snprintf(buf, sizeof(buf), "%u/%u", (unsigned)s_m.in_flight,
                 (unsigned)(s_m.slots ? s_m.slots : 0));
        label_text(s_con_val, buf);
        label_color(s_con_val, s_m.in_flight ? UI_ACCENT : UI_TEXT);
        if (s_con_bar) {
            lv_bar_set_range(s_con_bar, 0, s_m.slots ? s_m.slots : 1);
            lv_bar_set_value(s_con_bar, s_m.in_flight, LV_ANIM_OFF);
        }
    } else {
        label_text(s_con_val, "--");
        label_color(s_con_val, UI_TEXT_MUT);
    }

    if (s_m.has_queue) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)s_m.queued);
        label_text(s_queue_val, buf);
        label_color(s_queue_val, s_m.queued ? UI_WARN : UI_TEXT);
        if (s_queue_bar) lv_bar_set_value(s_queue_bar, s_m.queued, LV_ANIM_OFF);
    } else {
        label_text(s_queue_val, "--");
        label_color(s_queue_val, UI_TEXT_MUT);
    }

    if (s_m.has_kv) {
        snprintf(buf, sizeof(buf), "%.1f%%", (double)s_m.kv_pct);
        label_text(s_kv_val, buf);
        label_color(s_kv_val, s_m.kv_pct >= 90 ? UI_ERR :
                    (s_m.kv_pct >= 75 ? UI_WARN : UI_CYAN));
        if (s_kv_bar) {
            lv_bar_set_range(s_kv_bar, 0, 1000);
            lv_bar_set_value(s_kv_bar, (int32_t)(s_m.kv_pct * 10), LV_ANIM_OFF);
        }
    } else {
        label_text(s_kv_val, "--");
        label_color(s_kv_val, UI_TEXT_MUT);
    }

    if (s_m.has_mtp) {
        snprintf(buf, sizeof(buf), "%.0f%%", (double)s_m.mtp_pct);
        label_text(s_mtp_val, buf);
        label_color(s_mtp_val, s_m.mtp_pct >= 50 ? UI_OK : UI_ACCENT_2);
        if (s_mtp_bar) {
            lv_bar_set_range(s_mtp_bar, 0, 1000);
            lv_bar_set_value(s_mtp_bar, (int32_t)(s_m.mtp_pct * 10), LV_ANIM_OFF);
        }
    } else {
        label_text(s_mtp_val, "--");
        label_color(s_mtp_val, UI_TEXT_MUT);
    }

    char tok[24], cac[24], req[24];
    if (s_m.has_usage) {
        fmt_count(tok, sizeof(tok), s_m.tokens_in + s_m.tokens_out);
    } else {
        snprintf(tok, sizeof(tok), "--");
    }
    if (s_m.has_cache) {
        snprintf(cac, sizeof(cac), "%.0f%%", (double)s_m.cache_hit_pct);
    } else {
        snprintf(cac, sizeof(cac), "--");
    }
    if (s_m.has_requests) {
        fmt_count(req, sizeof(req), s_m.requests_total);
    } else {
        snprintf(req, sizeof(req), "--");
    }
    char ubuf[128];
    snprintf(ubuf, sizeof(ubuf), "累计 %s · 缓存 %s · 请求 %s", tok, cac, req);
    label_text(s_usage_lbl, ubuf);
    label_color(s_usage_lbl, s_m.online ? UI_TEXT_DIM : UI_TEXT_MUT);
}

static void update_chart_data(bool new_sample)
{
    if (!s_chart[0] || s_h.count == 0) return;

    if (new_sample) {
        int32_t v;
        v = (int32_t)(s_h.tps_dec[s_h.count - 1] * 10.0f + 0.5f);
        lv_chart_set_next_value(s_chart[0], s_chart_ser1[0], v);
        if (s_chart_ser2[0]) {
            v = (int32_t)(s_h.tps_pre[s_h.count - 1] * 10.0f + 0.5f);
            lv_chart_set_next_value(s_chart[0], s_chart_ser2[0], v);
        }
        lv_chart_set_next_value(s_chart[1], s_chart_ser1[1],
                                (int32_t)(s_h.kv[s_h.count - 1] * 10.0f + 0.5f));
        lv_chart_set_next_value(s_chart[2], s_chart_ser1[2],
                                (int32_t)(s_h.conc[s_h.count - 1] + 0.5f));
        lv_chart_set_next_value(s_chart[3], s_chart_ser1[3],
                                (int32_t)(s_h.power[s_h.count - 1] + 0.5f));

        lv_chart_refresh(s_chart[0]);
        lv_chart_refresh(s_chart[1]);
        lv_chart_refresh(s_chart[2]);
        lv_chart_refresh(s_chart[3]);
    }

    /* TPS / KV 序列内部按 x10 存储，纵轴范围同样放大。
       预填充量级远大于解码，单独走副轴，避免把解码曲线压扁。 */
    lv_chart_set_range(s_chart[0], LV_CHART_AXIS_PRIMARY_Y, 0,
                       (int32_t)(tps_scale() * 10.0f + 0.5f));
    lv_chart_set_range(s_chart[0], LV_CHART_AXIS_SECONDARY_Y, 0,
                       (int32_t)(pre_scale() * 10.0f + 0.5f));
    lv_chart_set_range(s_chart[1], LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_range(s_chart[2], LV_CHART_AXIS_PRIMARY_Y, 0,
                       (int32_t)(conc_scale() + 0.5f));
    lv_chart_set_range(s_chart[3], LV_CHART_AXIS_PRIMARY_Y, 0,
                       (int32_t)(power_scale() + 0.5f));
}

static void update_system(void)
{
    char buf[64];

    if (s_m.has_power) {
        snprintf(buf, sizeof(buf), "%.0f", (double)s_m.power_w);
        label_text(s_sys_power, buf);
        label_color(s_sys_power, s_m.power_w > 120 ? UI_ERR : UI_WARN);
        label_text(s_sys_power_sub, "整机功耗");
    } else {
        label_text(s_sys_power, "--");
        label_color(s_sys_power, UI_TEXT_MUT);
        label_text(s_sys_power_sub, "无数据");
    }

    if (s_m.has_temp) {
        snprintf(buf, sizeof(buf), "%.0f", (double)s_m.temp_c);
        label_text(s_sys_temp, buf);
        label_color(s_sys_temp, s_m.temp_c >= 90 ? UI_ERR :
                    (s_m.temp_c >= 75 ? UI_WARN : UI_OK));
        label_text(s_sys_temp_sub, "GPU / 封装");
    } else {
        label_text(s_sys_temp, "--");
        label_color(s_sys_temp, UI_TEXT_MUT);
        label_text(s_sys_temp_sub, "无数据");
    }

    if (s_m.has_gpu) {
        snprintf(buf, sizeof(buf), "%.0f", (double)s_m.gpu_pct);
        label_text(s_sys_gpu, buf);
        label_color(s_sys_gpu, s_m.gpu_pct >= 90 ? UI_WARN : UI_OK);
        label_text(s_sys_gpu_sub, "占用率");
    } else {
        label_text(s_sys_gpu, "--");
        label_color(s_sys_gpu, UI_TEXT_MUT);
        label_text(s_sys_gpu_sub, "无数据");
    }

    if (s_m.has_ram) {
        float pct = 0;
        if (s_m.ram_total_mb) pct = 100.0f * (float)s_m.ram_used_mb / (float)s_m.ram_total_mb;
        snprintf(buf, sizeof(buf), "%.0f", (double)pct);
        label_text(s_sys_ram, buf);
        label_color(s_sys_ram, pct >= 90 ? UI_ERR : (pct >= 75 ? UI_WARN : UI_CYAN));
        snprintf(buf, sizeof(buf), "%uG / %uG",
                 (unsigned)(s_m.ram_used_mb / 1024),
                 (unsigned)(s_m.ram_total_mb / 1024));
        label_text(s_sys_ram_sub, buf);
    } else if (s_m.has_vram) {
        float pct = 0;
        if (s_m.vram_total_mb) pct = 100.0f * (float)s_m.vram_used_mb / (float)s_m.vram_total_mb;
        snprintf(buf, sizeof(buf), "%.0f", (double)pct);
        label_text(s_sys_ram, buf);
        label_color(s_sys_ram, pct >= 90 ? UI_ERR : UI_CYAN);
        snprintf(buf, sizeof(buf), "%uG / %uG",
                 (unsigned)(s_m.vram_used_mb / 1024),
                 (unsigned)(s_m.vram_total_mb / 1024));
        label_text(s_sys_ram_sub, buf);
    } else {
        label_text(s_sys_ram, "--");
        label_color(s_sys_ram, UI_TEXT_MUT);
        label_text(s_sys_ram_sub, "无数据");
    }

    if (s_m.has_host_uptime) fmt_uptime(buf, sizeof(buf), s_m.host_uptime_s);
    else snprintf(buf, sizeof(buf), "--");
    label_text(s_up_host, buf);

    if (s_m.has_model_uptime) fmt_uptime(buf, sizeof(buf), s_m.model_uptime_s);
    else snprintf(buf, sizeof(buf), "--");
    label_text(s_up_model, buf);

    if (s_m.has_engine_time) fmt_uptime(buf, sizeof(buf), s_m.engine_time_s);
    else snprintf(buf, sizeof(buf), "--");
    label_text(s_up_engine, buf);

    fmt_uptime(buf, sizeof(buf), s_m.device_uptime_s);
    label_text(s_up_device, buf);

    if (s_m.has_cache) {
        snprintf(buf, sizeof(buf), "命中 %.1f%%", (double)s_m.cache_hit_pct);
        label_text(s_cache_hit, buf);
        fmt_bytes(buf, sizeof(buf), s_m.cache_bytes);
        char b2[80];
        snprintf(b2, sizeof(b2), "容量 %s", buf);
        label_text(s_cache_size, b2);
        snprintf(b2, sizeof(b2), "条目 %u", (unsigned)s_m.cache_entries);
        label_text(s_cache_entries, b2);
        char tok[24];
        fmt_count(tok, sizeof(tok), s_m.tokens_cached);
        snprintf(b2, sizeof(b2), "缓存 %s", tok);
        label_text(s_cache_disk, b2);
    } else {
        label_text(s_cache_hit, "命中 --");
        label_text(s_cache_size, "容量 --");
        label_text(s_cache_entries, "条目 --");
        label_text(s_cache_disk, "缓存 --");
    }

    snprintf(buf, sizeof(buf), "Wi-Fi %d  ·  堆 %uKB  ·  芯片 %d℃",
             (int)s_m.wifi_rssi, (unsigned)s_m.free_heap_kb,
             (int)s_m.device_temp_c);
    label_text(s_sys_device, buf);
}

void ui_mask_key(const char * key, char * out, size_t n)
{
    size_t len = key ? strlen(key) : 0;
    if (len == 0) {
        snprintf(out, n, "未设置");
    } else if (len <= 10) {
        snprintf(out, n, "已设置");
    } else {
        snprintf(out, n, "%.6s…%.4s", key, key + len - 4);
    }
}

static void update_settings(void)
{
    char buf[128];
    label_text(s_set_ssid, s_m.wifi_ssid[0] ? s_m.wifi_ssid : "未连接");
    label_text(s_set_ip, s_m.wifi_ip[0] ? s_m.wifi_ip : "--");
    snprintf(buf, sizeof(buf), "%d dBm", (int)s_m.wifi_rssi);
    label_text(s_set_rssi, s_m.wifi_rssi ? buf : "--");

    if (s_m.wifi_ip[0]) {
        label_text(s_set_net_state, s_m.online ? "已连接 · 数据正常" : "已连接 · 服务不可达");
        label_color(s_set_net_state, s_m.online ? UI_OK : UI_WARN);
    } else {
        label_text(s_set_net_state, "未连接 Wi-Fi");
        label_color(s_set_net_state, UI_ERR);
    }

    label_text(s_set_provider, s_m.provider_name[0] ? s_m.provider_name : "自动识别");
    label_text(s_set_endpoint, s_m.endpoint[0] ? s_m.endpoint : "未设置");
    label_text(s_set_key, s_m.api_key_mask[0] ? s_m.api_key_mask : "未设置");
    label_text(s_set_host, s_m.host_agent[0] ? s_m.host_agent : "未配置");
    label_color(s_set_host, s_m.host_agent[0] ? UI_CYAN : UI_TEXT_MUT);

    label_text(s_set_version, DASH_FW_VERSION);
}

void dashboard_ui_apply(const dash_metrics_t * m, bool new_sample)
{
    if (!m) return;
    s_m = *m;
    if (new_sample) history_push();

    update_header();
    update_overview();
    update_chart_data(new_sample);
    update_charts();
    update_system();
    update_settings();

    if (new_sample) {
        lv_chart_set_range(s_hero_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                           (int32_t)(tps_scale() * 10.0f + 0.5f));
        lv_chart_set_next_value(s_hero_chart, s_hero_ser,
                                (int32_t)((s_m.has_tps ? s_m.tps_dec : 0) * 10.0f));
        lv_chart_refresh(s_hero_chart);
    }
}

/* ============================== 配网浮层 / Toast ============================== */

void dashboard_ui_show_provisioning(bool show, const char * ssid,
                                    const char * password, const char * ip)
{
    if (!s_overlay) {
        lv_obj_t * top = lv_layer_top();
        s_overlay = lv_obj_create(top);
        lv_obj_remove_style_all(s_overlay);
        lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_overlay, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(s_overlay, 20, 0);
        lv_obj_set_style_pad_row(s_overlay, 10, 0);

        lv_obj_t * kicker = ui_cjk_create(s_overlay, "ESP32 配网");
        label_color(kicker, UI_ACCENT);

        lv_obj_t * title = lv_label_create(s_overlay);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
        label_color(title, UI_TEXT);
        lv_label_set_text(title, "SETUP MODE");

        lv_obj_t * card = ui_card_create(s_overlay);
        lv_obj_set_size(card, LV_PCT(100), 250);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_pad_row(card, 8, 0);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t * r1 = ui_group_create(card);
        lv_obj_set_size(r1, LV_PCT(100), 44);
        lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t * c1 = ui_caption_create(r1, "热点名称");
        label_color(c1, UI_TEXT_MUT);
        s_prov_ssid = lv_label_create(r1);
        lv_obj_set_style_text_font(s_prov_ssid, &lv_font_montserrat_20, 0);
        label_color(s_prov_ssid, UI_TEXT);
        lv_label_set_text(s_prov_ssid, "--");

        lv_obj_t * r2 = ui_group_create(card);
        lv_obj_set_size(r2, LV_PCT(100), 44);
        lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t * c2 = ui_caption_create(r2, "热点密码");
        label_color(c2, UI_TEXT_MUT);
        s_prov_pass = lv_label_create(r2);
        lv_obj_set_style_text_font(s_prov_pass, &lv_font_montserrat_20, 0);
        label_color(s_prov_pass, UI_TEXT);
        lv_label_set_text(s_prov_pass, "--");

        lv_obj_t * r3 = ui_group_create(card);
        lv_obj_set_size(r3, LV_PCT(100), 52);
        lv_obj_set_flex_flow(r3, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(r3, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t * c3 = ui_caption_create(r3, "浏览器打开");
        label_color(c3, UI_TEXT_MUT);
        s_prov_url = lv_label_create(r3);
        lv_obj_set_style_text_font(s_prov_url, &lv_font_montserrat_24, 0);
        label_color(s_prov_url, UI_ACCENT);
        lv_label_set_text(s_prov_url, "192.168.4.1");

        lv_obj_t * steps = ui_cjk_create(card,
            "1. 手机连接上方热点\n"
            "2. 浏览器访问 192.168.4.1\n"
            "3. 选择 Wi-Fi 并填写服务地址");
        lv_obj_set_style_text_align(steps, LV_TEXT_ALIGN_CENTER, 0);
        label_color(steps, UI_TEXT_DIM);

        s_prov_state = ui_cjk_create(card, "等待手机配置…");
        label_color(s_prov_state, UI_WARN);
    }

    if (ssid) label_text(s_prov_ssid, ssid);
    if (password) label_text(s_prov_pass, password);
    if (ip) label_text(s_prov_url, ip);

    if (show) lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void toast_timer_cb(lv_timer_t * t)
{
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    s_toast_timer = NULL;
}

void dashboard_ui_show_toast(const char * text)
{
    if (!s_toast) {
        s_toast = ui_group_create(lv_layer_top());
        lv_obj_set_size(s_toast, LV_SIZE_CONTENT, 32);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(UI_SURFACE_2), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
        lv_obj_set_style_radius(s_toast, UI_R_PILL, 0);
        lv_obj_set_style_border_width(s_toast, 1, 0);
        lv_obj_set_style_border_color(s_toast, lv_color_hex(UI_BORDER), 0);
        lv_obj_set_style_pad_hor(s_toast, 12, 0);
        lv_obj_set_style_pad_ver(s_toast, 4, 0);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -64);
        lv_obj_t * l = lv_label_create(s_toast);
        lv_obj_set_style_text_font(l, UI_F_CJK, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
        lv_label_set_text(l, text);
    } else {
        lv_obj_t * l = lv_obj_get_child(s_toast, 0);
        label_text(l, text);
    }
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) lv_timer_del(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_timer_cb, 2500, NULL);
}

/* ============================== 创建 ============================== */

static void clock_timer_cb(lv_timer_t * t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    if (now > 1700000000 && localtime_r(&now, &tm)) {
        strftime(s_clock_buf, sizeof(s_clock_buf), "%H:%M", &tm);
    } else {
        snprintf(s_clock_buf, sizeof(s_clock_buf), "--:--");
    }
    update_top_right();

    if (s_status_dot && s_m.online && s_m.busy) {
        uint32_t phase = (uint32_t)(lv_tick_get() / 400U) % 2U;
        lv_obj_set_style_opa(s_status_dot, phase ? LV_OPA_40 : LV_OPA_COVER, 0);
    } else if (s_status_dot) {
        lv_obj_set_style_opa(s_status_dot, LV_OPA_COVER, 0);
    }
}

static void build_nav(lv_obj_t * parent)
{
    lv_obj_t * nav = ui_group_create(parent);
    lv_obj_set_size(nav, LV_PCT(100), 48);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char * names[4] = {"概览", "曲线", "系统", "设置"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t * b = ui_group_create(nav);
        lv_obj_set_size(b, LV_PCT(25) - 3, 40);
        lv_obj_set_style_radius(b, UI_R_M, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(b);
        lv_obj_set_style_text_font(l, UI_F_CJK, 0);
        lv_label_set_text(l, names[i]);
        label_color(l, UI_TEXT_DIM);
        s_nav_btn[i] = b;
        s_nav_lbl[i] = l;
    }
}

void dashboard_ui_create(void)
{
    memset(&s_h, 0, sizeof(s_h));
    memset(&s_m, 0, sizeof(s_m));
    s_m.device_uptime_s = 0;

    lv_obj_t * scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, PAGE_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, PAGE_PAD, 0);
    lv_obj_set_style_pad_row(scr, PAGE_GAP, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(scr);
    build_header(scr);

    lv_obj_t * content = ui_group_create(scr);
    lv_obj_set_size(content, LV_PCT(100), CONTENT_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t * p = ui_group_create(content);
        lv_obj_set_size(p, LV_PCT(100), CONTENT_H);
        lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(p, 8, 0);
        s_pages[i] = p;
    }
    build_overview(s_pages[0]);
    build_charts(s_pages[1]);
    build_system(s_pages[2]);
    build_settings(s_pages[3]);

    build_nav(scr);

    dashboard_ui_set_page(0);
    set_seg_active();
    lv_timer_create(clock_timer_cb, 1000, NULL);
}
