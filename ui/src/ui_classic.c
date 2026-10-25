/*
 * 传统框架（classic）固定式界面渲染器。
 *
 * 横屏 480×320 基准坐标，按 canvas 实际尺寸线性缩放。
 * 主题来源（docs/design/）：
 *   instrument  第 2 组 I01 磷光示波器
 *   hud         第 6 组 H01 战术 HUD
 *   mecha       第 6 组 H02 机甲反应堆
 *   stealth     第 6 组 H03 潜行终端
 *   arcade      第 6 组 H04 街机计分板
 *   drone       第 6 组 H05 无人机链路
 *   scope       第 6 组 H06 狙击镜（竖稿横排适配）
 *   heatsink    第 6 组 H07 散热塔（竖稿横排适配）
 *   loadout     第 6 组 H08 装备槽（竖稿横排适配）
 *   armor       第 6 组 H09 动力装甲（竖稿横排适配）
 *   sweep       第 6 组 H10 战术雷达（竖稿横排适配）
 *
 * 字体只允许 Montserrat 14/16/20/24/28/32/40/48（固件 sdkconfig 与
 * sim/lv_conf.h 均已启用）。
 */
#include "ui_classic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dash_metric_store.h"
#include "lvgl.h"
#include "ui.h"

/* ============================ 指标订阅集合 ============================ */

static const char *s_metric_ids[] = {
    "llm.model",     "llm.online",   "llm.busy",     "llm.phase",
    /* 三颗服务 LED：引擎侧三态（在线 / 前缀缓存启用 / 草稿权重已加载） */
    "llm.cache_enabled", "llm.drafter_loaded",
    "host.gpu_util_pct", "host.cpu_pct",
    "host.ram_used_mb",  "host.ram_total_mb",
    /* 6 路仪表：KV 占用 / 前缀缓存命中率 / 草稿接受率 */
    "llm.kv_pct", "llm.cache_token_hit_rate", "llm.draft_accept_pct",
    "llm.kv_used_tokens", "llm.kv_pool_positions",
    "llm.slots",  "llm.requests_deferred",
    "host.gpu_temp_c", "host.gpu_power_w",
    /* H02-H10 主题额外使用 */
    "llm.decode_tps", "llm.prefill_tps", "llm.decode_avg",
    "llm.decode_peak",
    "host.gpu_vram_used_mb", "host.gpu_vram_total_mb",
    "llm.context_len",
    /* loadout 卡 04 的缓存副行 / heatsink 第三柱的草稿 token 副行 */
    "llm.cache_bytes", "llm.cache_cap_mb", "llm.cache_entries",
    "llm.draft_tokens",
};

const char **ui_classic_metric_ids(int *count)
{
    if (count) *count = (int)(sizeof(s_metric_ids) / sizeof(s_metric_ids[0]));
    return s_metric_ids;
}

/* ============================ 主题调色板 ============================ */

enum {
    TH_INSTRUMENT = 0, TH_HUD, TH_MECHA, TH_STEALTH, TH_ARCADE, TH_DRONE,
    TH_SCOPE, TH_HEATSINK, TH_LOADOUT, TH_ARMOR, TH_SWEEP, TH_COUNT
};

typedef struct {
    uint32_t bg, bg2;
    uint32_t surface, border;
    uint32_t text, dim, mut;
    uint32_t acc, acc2, acc3;
    uint32_t warn, err;
    uint32_t track;
} pal_t;

static const pal_t s_pal[TH_COUNT] = {
    /* instrument：磷光示波器（第 2 组 I01） */
    {0x07110B, 0x0A1911, 0x08150E, 0x163626, 0xD9F8E6, 0x4D8B6A, 0x2F5A44,
     0x38F28A, 0xA9FFCE, 0xFFD166, 0xFFD166, 0xFF5347, 0x1C3A28},
    /* hud：战术 HUD（第 6 组 H01） */
    {0x0A0F0C, 0x0D1512, 0x12201A, 0x27402D, 0xD8F5D0, 0x6D8673, 0x3E5740,
     0x7CFF4F, 0xFFB020, 0x3BE0C0, 0xFFB020, 0xFF5347, 0x1B2F21},
    /* mecha：机甲反应堆（H02，橙/青） */
    {0x0E1116, 0x121A24, 0x10161D, 0x232D3A, 0xE8F0F7, 0x6B7A8C, 0x3D4754,
     0xFF6B35, 0x00D1FF, 0x9DB4C8, 0xFFB020, 0xFF5347, 0x161D27},
    /* stealth：潜行终端（H03，灰绿极简） */
    {0x14171A, 0x111417, 0x121518, 0x28322E, 0xD9E5DE, 0x5F6B66, 0x3A423E,
     0x4ADE80, 0x9CA3AF, 0xA7C4B5, 0xD9B44A, 0xFF5347, 0x1D2422},
    /* arcade：街机计分板（H04，黄/品红） */
    {0x120A1C, 0x1B0F2A, 0x170D24, 0x3D2660, 0xFFF6D6, 0x8A7AA8, 0x57496F,
     0xFFCF00, 0xFF3EA5, 0x7EF9FF, 0xFFCF00, 0xFF5347, 0x2C1B42},
    /* drone：无人机链路（H05，荧光绿） */
    {0x07110D, 0x0A1A14, 0x0A1610, 0x1A3A2C, 0xD6FFF0, 0x4E7A68, 0x2F4F42,
     0x00FF9D, 0xFFB020, 0x7DFFD4, 0xFFB020, 0xFF5347, 0x12261E},
    /* scope：狙击镜（H06，同心刻度） */
    {0x07110D, 0x08170F, 0x0A170F, 0x1B3E2C, 0xD8FFF0, 0x3F6B57, 0x27493A,
     0x00FF9D, 0x9FFFE0, 0xFFB020, 0xFFB020, 0xFF5347, 0x122A1F},
    /* heatsink：散热塔（H07，橙/青纵柱） */
    {0x0E1116, 0x131A22, 0x10161D, 0x232D3A, 0xE8F0F7, 0x6B7A8C, 0x3D4754,
     0xFF6B35, 0x00D1FF, 0xFFD166, 0xFFB020, 0xFF5347, 0x1B232E},
    /* loadout：装备槽（H08，绿四卡） */
    {0x0A0F0C, 0x0E1712, 0x0D1712, 0x22381F, 0xD8F5D0, 0x5D7A5A, 0x3A5240,
     0x7CFF4F, 0xFFB020, 0x3BE0C0, 0xFFB020, 0xFF5347, 0x1A2A1C},
    /* armor：动力装甲（H09，橙/青能量核心） */
    {0x0E1116, 0x131B24, 0x10161D, 0x232D3A, 0xE8F0F7, 0x6B7A8C, 0x3D4754,
     0xFF6B35, 0x00D1FF, 0xFFD166, 0xFFB020, 0xFF5347, 0x1B232E},
    /* sweep：战术雷达（H10，绿扫描） */
    {0x0A0F0C, 0x0D1611, 0x0B130E, 0x22381F, 0xD8F5D0, 0x5D7A5A, 0x3A5240,
     0x7CFF4F, 0xFFB020, 0x3BE0C0, 0xFFB020, 0xFF5347, 0x1A2A1C},
};

/* ============================ 内部状态 ============================ */

#define N_PCT 6  /* GPU CPU KV MEM HIT ACC */
#define N_SEG 10
#define N_ROW 8  /* 通用行/段槽位 */

typedef struct {
    lv_obj_t *segs[N_SEG];
    int n;
    uint32_t dim;
} segbar_t;

struct ui_classic {
    int theme;
    float sx, sy;
    lv_obj_t *screen;

    lv_obj_t *led_main;
    lv_obj_t *lbl_model_top;
    lv_obj_t *lbl_phase_top;

    lv_obj_t *pct_bar[N_PCT]; /* 连续条填充（instrument） */
    int pct_bar_w[N_PCT];     /* 轨道像素宽（创建期查询不可靠，显式记录） */
    lv_obj_t *pct_val[N_PCT];
    segbar_t pct_seg[N_PCT]; /* 分段条（hud） */

    lv_obj_t *arc_gpu; /* GPU / CPU 环 */
    lv_obj_t *arc_gpu_val;
    lv_obj_t *arc_kv; /* 内环（mecha / armor） */

    lv_obj_t *lbl_big[3]; /* 主题主值大字（decode 等） */

    lv_obj_t *row_val[N_ROW]; /* 通用行值 */
    lv_obj_t *row_bar[N_ROW]; /* 通用行连续条 */
    int row_bar_w[N_ROW];
    segbar_t row_seg[N_ROW]; /* 通用行分段条 */

    lv_obj_t *txt_lbl[8]; /* 杂项动态文本 */
    lv_obj_t *lbl_temp;
    lv_obj_t *lbl_power;

    lv_obj_t *led_svc[3];
    lv_obj_t *lbl_svc[3];

    lv_obj_t *lbl_model;
    lv_obj_t *lbl_conc;
    lv_obj_t *lbl_queue;
    lv_obj_t *lbl_kv_text;
    lv_obj_t *press_lbl; /* arcade PRESS START 闪烁 */

    lv_timer_t *blink;
    bool blink_on;
    bool busy;
};

#define SX(c, v) ((int)lroundf((v) * (c)->sx))
#define SY(c, v) ((int)lroundf((v) * (c)->sy))

/* ============================ 指标读取 ============================ */

static bool m_num(const char *id, double *out)
{
    const dash_metric_t *m = dash_metric_store_find(id);
    if (!m || !m->available || m->type != DASH_METRIC_NUMBER) return false;
    *out = m->number;
    return true;
}

static bool m_bool(const char *id, bool *out)
{
    const dash_metric_t *m = dash_metric_store_find(id);
    if (!m || !m->available || m->type != DASH_METRIC_BOOL) return false;
    *out = m->boolean;
    return true;
}

static void m_text(const char *id, char *buf, size_t cap,
                   const char *fallback)
{
    const dash_metric_t *m = dash_metric_store_find(id);
    if (m && m->available && m->type == DASH_METRIC_STRING && m->text[0]) {
        snprintf(buf, cap, "%s", m->text);
    } else {
        snprintf(buf, cap, "%s", fallback);
    }
}

/* 六个占用率：0 GPU 1 CPU 2 KV 3 MEM 4 HIT(前缀缓存) 5 ACC(草稿接受) */
static bool pct_value(int idx, double *out)
{
    double a, b;
    switch (idx) {
    case 0: return m_num("host.gpu_util_pct", out);
    case 1: return m_num("host.cpu_pct", out);
    case 2: return m_num("llm.kv_pct", out);
    case 3:
        if (!m_num("host.ram_used_mb", &a)) return false;
        if (!m_num("host.ram_total_mb", &b) || b <= 0.0) return false;
        *out = a / b * 100.0;
        return true;
    case 4: return m_num("llm.cache_token_hit_rate", out);
    case 5: return m_num("llm.draft_accept_pct", out);
    default: return false;
    }
}

static uint32_t pct_color(const pal_t *p, double v)
{
    if (v >= 90.0) return p->err;
    if (v >= 75.0) return p->warn;
    return p->acc;
}

/* HIT（缓存命中率）/ ACC（草稿接受率）越高越好，阈值方向相反 */
static uint32_t pct_color_hi(const pal_t *p, double v)
{
    if (v < 50.0) return p->err;
    if (v < 75.0) return p->warn;
    return p->acc;
}

/* 按仪表槽位取色：4=HIT 5=ACC 走高优先，其余走负载优先 */
static uint32_t pct_color_at(const pal_t *p, int idx, double v)
{
    return (idx == 4 || idx == 5) ? pct_color_hi(p, v) : pct_color(p, v);
}

static uint32_t temp_color(const pal_t *p, double t)
{
    if (t >= 85.0) return p->err;
    if (t >= 75.0) return p->warn;
    return p->text;
}

static void fmt_tokens(double v, char *buf, size_t cap)
{
    if (v >= 1000000.0) snprintf(buf, cap, "%.1fM", v / 1000000.0);
    else if (v >= 1000.0) snprintf(buf, cap, "%.1fK", v / 1000.0);
    else snprintf(buf, cap, "%.0f", v);
}

static double clamp_pct(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 100.0) return 100.0;
    return v;
}

/* ============================ 绘图助手 ============================ */

static lv_obj_t *mk_lbl(lv_obj_t *parent, int x, int y, const char *text,
                        const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_lbl_w(lv_obj_t *parent, int x, int y, int w,
                          const char *text, const lv_font_t *font,
                          uint32_t color, lv_text_align_t align)
{
    lv_obj_t *l = mk_lbl(parent, x, y, text, font, color);
    lv_obj_set_width(l, w);
    /* DOTS 模式需要固定高度，否则 LVGL 会退化成换行 */
    lv_obj_set_height(l, lv_font_get_line_height(font));
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static lv_obj_t *mk_card(ui_classic_t *c, int x, int y, int w, int h,
                         uint32_t bg, uint32_t border)
{
    lv_obj_t *o = lv_obj_create(c->screen);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, SX(c, x), SY(c, y));
    lv_obj_set_size(o, SX(c, w), SY(c, h));
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* 实心矩形（屏幕坐标缩放）：细线 / 像素方块 / 刻度 */
static lv_obj_t *mk_rect(ui_classic_t *c, lv_obj_t *parent, int x, int y,
                         int w, int h, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, SX(c, x), SY(c, y));
    lv_obj_set_size(o, SX(c, w) ? SX(c, w) : 1, SY(c, h) ? SY(c, h) : 1);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* 实心矩形（父容器内原始坐标，不二次缩放） */
static lv_obj_t *mk_rect_p(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w ? w : 1, h ? h : 1);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* 只描边矩形（父容器内原始坐标） */
static lv_obj_t *mk_box_p(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* 只描边圆环（屏幕坐标缩放）：雷达 / 装饰圈 */
static lv_obj_t *mk_circle(ui_classic_t *c, lv_obj_t *parent, int x, int y,
                           int size, uint32_t border)
{
    return mk_box_p(parent, SX(c, x), SY(c, y), SX(c, size), SY(c, size),
                    border);
}

/* 四角括号：4 个只描两条边的小方块（尺寸显式传入，创建期查询不可靠） */
static void mk_corners(ui_classic_t *c, lv_obj_t *card, int w, int h,
                       uint32_t color, int len)
{
    static const lv_border_side_t sides[4] = {
        LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP,
        LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP,
        LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM,
        LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM,
    };
    const int s = SX(c, len);
    const int pad_x = SX(c, 3), pad_y = SY(c, 3);
    const int cw = SX(c, w), ch = SY(c, h);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *o = lv_obj_create(card);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, s, s);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
        lv_obj_set_style_border_side(o, sides[i], 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        const int px = (i % 2 == 0) ? pad_x : cw - pad_x - s;
        const int py = (i < 2) ? pad_y : ch - pad_y - s;
        lv_obj_set_pos(o, px, py);
    }
}

static lv_obj_t *mk_led(ui_classic_t *c, lv_obj_t *parent, int x, int y,
                        int size, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, SX(c, x), SY(c, y));
    return o;
}

static void led_set(lv_obj_t *led, bool on, uint32_t on_color,
                    uint32_t off_color)
{
    if (!led) return;
    lv_obj_set_style_bg_color(led, lv_color_hex(on ? on_color : off_color), 0);
    lv_obj_set_style_opa(led, on ? LV_OPA_COVER : LV_OPA_40, 0);
}

/* 连续条：track + fill；返回 fill，轨道宽显式记录 */
static lv_obj_t *mk_bar(ui_classic_t *c, lv_obj_t *parent, int x, int y, int w,
                        int h, uint32_t track, uint32_t fill, int *w_out)
{
    const int tw = SX(c, w);
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_pos(t, SX(c, x), SY(c, y));
    lv_obj_set_size(t, tw, SY(c, h));
    lv_obj_set_style_bg_color(t, lv_color_hex(track), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *f = lv_obj_create(t);
    lv_obj_remove_style_all(f);
    lv_obj_set_pos(f, 0, 0);
    lv_obj_set_size(f, 0, LV_PCT(100));
    lv_obj_set_style_bg_color(f, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    if (w_out) *w_out = tw;
    return f;
}

static void bar_set(lv_obj_t *fill, int track_w, double pct, uint32_t color)
{
    if (!fill) return;
    pct = clamp_pct(pct);
    lv_obj_set_width(fill, (int)lroundf(track_w * (float)pct / 100.0f));
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
}

/* 分段条：hor = 横向弹药条，vert = 纵向能量柱 */
static void segbar_create_ex(ui_classic_t *c, segbar_t *sb, lv_obj_t *parent,
                             int x, int y, int w, int h, uint32_t dim, int n,
                             bool vert)
{
    if (n > N_SEG) n = N_SEG;
    sb->n = n;
    sb->dim = dim;
    const int gap = SX(c, 2);
    for (int i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_radius(o, 1, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(dim), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        if (vert) {
            const int sh = (SY(c, h) - (n - 1) * gap) / n;
            lv_obj_set_size(o, SX(c, w), sh);
            lv_obj_set_pos(o, SX(c, x), SY(c, y) + i * (sh + gap));
        } else {
            const int sw = (SX(c, w) - (n - 1) * gap) / n;
            lv_obj_set_size(o, sw, SY(c, h));
            lv_obj_set_pos(o, SX(c, x) + i * (sw + gap), SY(c, y));
        }
        sb->segs[i] = o;
    }
}

/* 横向 10 段弹药条（兼容旧签名） */
static void segbar_create(ui_classic_t *c, segbar_t *sb, lv_obj_t *parent,
                          int x, int y, int w, int h, uint32_t dim)
{
    segbar_create_ex(c, sb, parent, x, y, w, h, dim, N_SEG, false);
}

static void segbar_set(segbar_t *sb, double pct, uint32_t color)
{
    if (sb->n <= 0) return;
    const int on = (int)lroundf((float)sb->n * (float)clamp_pct(pct) / 100.0f);
    for (int i = 0; i < sb->n; i++) {
        lv_obj_set_style_bg_color(
            sb->segs[i], lv_color_hex(i < on ? color : sb->dim), 0);
    }
}

/* 270° 环形仪表，缺口朝下 */
static lv_obj_t *mk_arc(ui_classic_t *c, lv_obj_t *parent, int x, int y,
                        int size, int width, uint32_t color, uint32_t track)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(a, SX(c, x), SY(c, y));
    lv_obj_set_size(a, SX(c, size), SY(c, size));
    /* 135→45 顺时针 270°，缺口（45→135）正好落在正下方 */
    lv_arc_set_bg_angles(a, 135, 45);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_width(a, SX(c, width), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, SX(c, width), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    return a;
}

/* 360° 全环（狙击镜准星环） */
static lv_obj_t *mk_arc_full(ui_classic_t *c, lv_obj_t *parent, int x, int y,
                             int size, int width, uint32_t color,
                             uint32_t track)
{
    lv_obj_t *a = mk_arc(c, parent, x, y, size, width, color, track);
    lv_arc_set_bg_angles(a, 0, 359);
    return a;
}

/* ============================ 主题：instrument ============================ */

static const char *s_pct_labels[N_PCT] = {"GPU", "CPU", "KV",
                                          "MEM", "HIT", "ACC"};

/* 三颗服务 LED 的标签：引擎侧三态（在线 / 前缀缓存 / 草稿权重）。
 * LED 与 ON/OFF 之间只有 36~38px 余量，Montserrat 14 下必须收在 3 个字母。 */
static const char *s_svc_labels[3] = {"LLM", "PFX", "DRF"};

static void build_instrument(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_card(c, 0, 0, 480, 28, p->surface, p->border);
    c->led_main = mk_led(c, c->screen, 10, 10, 8, p->acc);
    mk_lbl(c->screen, 24, 6, "INFERENCE SCOPE", &lv_font_montserrat_16,
           p->acc);
    c->lbl_model_top = mk_lbl_w(c->screen, 192, 7, 130, "--",
                                &lv_font_montserrat_14, p->acc3,
                                LV_TEXT_ALIGN_LEFT);
    c->lbl_phase_top =
        mk_lbl_w(c->screen, 330, 7, 96, "--", &lv_font_montserrat_14, p->dim,
                 LV_TEXT_ALIGN_RIGHT);
    mk_lbl(c->screen, 440, 6, "RUN", &lv_font_montserrat_14, p->acc);

    /* 左卡：资源占用 */
    lv_obj_t *left = mk_card(c, 10, 34, 296, 168, p->surface, p->border);
    mk_lbl(left, 12, 6, "RESOURCE LOAD", &lv_font_montserrat_14, p->dim);
    for (int i = 0; i < N_PCT; i++) {
        const int y = 28 + i * 23;
        mk_lbl(left, 12, y, s_pct_labels[i], &lv_font_montserrat_14, p->dim);
        c->pct_bar[i] =
            mk_bar(c, left, 56, y + 3, 170, 8, p->track, p->acc,
                   &c->pct_bar_w[i]);
        c->pct_val[i] = mk_lbl_w(left, 236, y, 48, "--",
                                 &lv_font_montserrat_16, p->text,
                                 LV_TEXT_ALIGN_RIGHT);
    }

    /* 右卡：GPU 环 + 温度功率 */
    lv_obj_t *right = mk_card(c, 314, 34, 156, 168, p->surface, p->border);
    mk_lbl(right, 12, 6, "GPU LOAD", &lv_font_montserrat_14, p->dim);
    c->arc_gpu = mk_arc(c, right, 30, 22, 96, 8, p->acc, p->track);
    c->arc_gpu_val = mk_lbl_w(right, 30, 54, 96, "--", &lv_font_montserrat_28,
                              p->text, LV_TEXT_ALIGN_CENTER);
    mk_lbl(right, 14, 124, "TEMP", &lv_font_montserrat_14, p->dim);
    c->lbl_temp = mk_lbl(right, 14, 140, "--", &lv_font_montserrat_20, p->text);
    mk_lbl(right, 86, 124, "PWR", &lv_font_montserrat_14, p->dim);
    c->lbl_power =
        mk_lbl(right, 86, 140, "--", &lv_font_montserrat_20, p->text);

    /* 底卡：服务 / 模型 / 并发排队 */
    lv_obj_t *bottom = mk_card(c, 10, 210, 460, 100, p->surface, p->border);
    mk_lbl(bottom, 12, 6, "SERVICES", &lv_font_montserrat_14, p->dim);
    for (int i = 0; i < 3; i++) {
        const int y = 26 + i * 23;
        c->led_svc[i] = mk_led(c, bottom, 14, y + 3, 7, p->dim);
        mk_lbl(bottom, 28, y, s_svc_labels[i], &lv_font_montserrat_14, p->dim);
        c->lbl_svc[i] = mk_lbl(bottom, 66, y, "OFF", &lv_font_montserrat_14,
                               p->mut);
    }
    mk_lbl(bottom, 150, 6, "MODEL", &lv_font_montserrat_14, p->dim);
    c->lbl_model =
        mk_lbl_w(bottom, 150, 24, 130, "--", &lv_font_montserrat_20, p->acc2,
                 LV_TEXT_ALIGN_LEFT);
    mk_lbl(bottom, 300, 6, "CONC", &lv_font_montserrat_14, p->dim);
    c->lbl_conc = mk_lbl(bottom, 300, 24, "--", &lv_font_montserrat_28,
                         p->text);
    mk_lbl(bottom, 372, 6, "QUEUE", &lv_font_montserrat_14, p->dim);
    c->lbl_queue = mk_lbl(bottom, 372, 24, "--", &lv_font_montserrat_28,
                          p->text);
    mk_lbl(bottom, 150, 62, "KV POOL", &lv_font_montserrat_14, p->dim);
    c->lbl_kv_text = mk_lbl(bottom, 150, 80, "KV --", &lv_font_montserrat_16,
                            p->text);
}

/* ============================ 主题：hud ============================ */

static void build_hud(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_card(c, 0, 0, 480, 26, p->surface, p->border);
    c->led_main = mk_led(c, c->screen, 9, 9, 8, p->acc);
    mk_lbl(c->screen, 22, 5, "TACTICAL HUD", &lv_font_montserrat_16, p->acc);
    c->lbl_model_top = mk_lbl_w(c->screen, 152, 6, 140, "// --",
                                &lv_font_montserrat_14, p->text,
                                LV_TEXT_ALIGN_LEFT);
    c->lbl_phase_top =
        mk_lbl_w(c->screen, 300, 6, 100, "--", &lv_font_montserrat_14, p->dim,
                 LV_TEXT_ALIGN_RIGHT);
    mk_lbl(c->screen, 416, 5, "ONLINE", &lv_font_montserrat_14, p->acc);

    /* 卡 A：GPU 环 */
    lv_obj_t *ca = mk_card(c, 8, 30, 140, 134, p->surface, p->border);
    mk_corners(c, ca, 140, 134, p->acc, 9);
    mk_lbl(ca, 16, 6, "GPU", &lv_font_montserrat_14, p->dim);
    c->arc_gpu = mk_arc(c, ca, 24, 24, 92, 7, p->acc, p->track);
    c->arc_gpu_val = mk_lbl_w(ca, 24, 56, 92, "--", &lv_font_montserrat_28,
                              p->acc, LV_TEXT_ALIGN_CENTER);

    /* 卡 B：分段条负载（CPU MEM HIT ACC）。GPU 走卡 A 的环，KV 走卡 C，
     * 卡 C 的标签槽只有 26px，放不下 "ACC"，故 ACC 排到卡 B 第 4 行。 */
    static const int b_idx[4] = {1, 3, 4, 5};
    lv_obj_t *cb = mk_card(c, 154, 30, 166, 134, p->surface, p->border);
    mk_corners(c, cb, 166, 134, p->acc, 9);
    mk_lbl(cb, 16, 6, "LOAD", &lv_font_montserrat_14, p->dim);
    for (int i = 0; i < 4; i++) {
        const int idx = b_idx[i];
        const int y = 26 + i * 26;
        mk_lbl(cb, 14, y, s_pct_labels[idx], &lv_font_montserrat_14, p->dim);
        segbar_create(c, &c->pct_seg[idx], cb, 50, y + 2, 70, 10, p->track);
        c->pct_val[idx] = mk_lbl_w(cb, 124, y, 36, "--",
                                   &lv_font_montserrat_14, p->text,
                                   LV_TEXT_ALIGN_RIGHT);
    }

    /* 卡 C：温度 / 功率 / KV */
    lv_obj_t *cc = mk_card(c, 326, 30, 146, 134, p->surface, p->border);
    mk_corners(c, cc, 146, 134, p->acc, 9);
    mk_lbl(cc, 16, 6, "THERMAL", &lv_font_montserrat_14, p->dim);
    c->lbl_temp = mk_lbl(cc, 10, 24, "--", &lv_font_montserrat_32, p->acc2);
    mk_lbl(cc, 78, 38, "C", &lv_font_montserrat_14, p->dim);
    c->lbl_power = mk_lbl(cc, 10, 60, "--", &lv_font_montserrat_32, p->acc3);
    mk_lbl(cc, 92, 74, "W", &lv_font_montserrat_14, p->dim);
    mk_lbl(cc, 10, 100, "KV", &lv_font_montserrat_14, p->dim);
    segbar_create(c, &c->pct_seg[2], cc, 36, 102, 70, 8, p->track);
    c->pct_val[2] = mk_lbl_w(cc, 112, 100, 28, "--", &lv_font_montserrat_14,
                             p->text, LV_TEXT_ALIGN_RIGHT);

    /* 卡 D：服务 + 模型 */
    lv_obj_t *cd = mk_card(c, 8, 170, 240, 142, p->surface, p->border);
    mk_corners(c, cd, 240, 142, p->acc, 9);
    mk_lbl(cd, 16, 6, "SERVICES", &lv_font_montserrat_14, p->dim);
    for (int i = 0; i < 3; i++) {
        const int y = 26 + i * 24;
        c->led_svc[i] = mk_led(c, cd, 12, y + 3, 7, p->dim);
        mk_lbl(cd, 26, y, s_svc_labels[i], &lv_font_montserrat_14, p->dim);
        c->lbl_svc[i] = mk_lbl(cd, 62, y, "OFF", &lv_font_montserrat_14,
                               p->mut);
    }
    mk_lbl(cd, 10, 104, "MODEL", &lv_font_montserrat_14, p->dim);
    c->lbl_model = mk_lbl_w(cd, 66, 104, 164, "--", &lv_font_montserrat_16,
                            p->text, LV_TEXT_ALIGN_LEFT);

    /* 卡 E：并发 / 排队 */
    lv_obj_t *ce = mk_card(c, 254, 170, 218, 142, p->surface, p->border);
    mk_corners(c, ce, 218, 142, p->acc, 9);
    mk_lbl(ce, 16, 6, "THROUGHPUT", &lv_font_montserrat_14, p->dim);
    c->lbl_conc = mk_lbl(ce, 10, 28, "--", &lv_font_montserrat_40, p->acc);
    mk_lbl(ce, 10, 72, "CONC", &lv_font_montserrat_14, p->dim);
    c->lbl_queue = mk_lbl(ce, 118, 28, "--", &lv_font_montserrat_40, p->acc2);
    mk_lbl(ce, 118, 72, "QUEUE", &lv_font_montserrat_14, p->dim);
    c->lbl_kv_text = mk_lbl(ce, 10, 108, "KV --", &lv_font_montserrat_14,
                            p->dim);
}

/* ============================ 主题：mecha（H02 机甲反应堆） ============================ */

static void build_mecha(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_card(c, 0, 0, 480, 26, p->surface, p->border);
    c->led_main = mk_led(c, c->screen, 10, 9, 8, p->acc);
    mk_lbl(c->screen, 24, 5, "MECHA CORE", &lv_font_montserrat_16, p->acc);
    c->lbl_model_top = mk_lbl_w(c->screen, 140, 6, 120, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_LEFT);
    c->lbl_phase_top = mk_lbl_w(c->screen, 264, 6, 72, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    c->led_svc[1] = mk_led(c, c->screen, 354, 9, 8, p->dim);
    c->led_svc[2] = mk_led(c, c->screen, 372, 9, 8, p->dim);
    mk_lbl(c->screen, 386, 6, "SYS", &lv_font_montserrat_14, p->dim);
    mk_lbl_w(c->screen, 404, 6, 72, "UNIT-07", &lv_font_montserrat_14,
             p->acc2, LV_TEXT_ALIGN_RIGHT);

    /* 顶部整体负载条（56 段） */
    segbar_create_ex(c, &c->row_seg[0], c->screen, 18, 44, 444, 4, p->track,
                     56, false);

    /* 反应堆：外环 GPU / 内环 ACC / 中心 DECODE */
    c->arc_gpu = mk_arc(c, c->screen, 145, 62, 190, 7, p->acc, p->track);
    c->arc_kv = mk_arc(c, c->screen, 174, 91, 132, 3, p->acc2, p->track);
    mk_lbl_w(c->screen, 190, 124, 100, "DECODE", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);
    c->lbl_big[0] = mk_lbl_w(c->screen, 190, 142, 100, "--",
                             &lv_font_montserrat_40, p->text,
                             LV_TEXT_ALIGN_CENTER);
    mk_lbl_w(c->screen, 190, 186, 100, "TOK/S", &lv_font_montserrat_14,
             p->acc, LV_TEXT_ALIGN_CENTER);

    /* 左列 4 行：PREFILL / SLOTS / VRAM / ACC */
    static const char *l_rows[4] = {"PREFILL", "SLOTS", "VRAM", "ACC"};
    for (int i = 0; i < 4; i++) {
        const int y = 64 + i * 50;
        mk_lbl(c->screen, 14, y + 1, l_rows[i], &lv_font_montserrat_14,
               p->dim);
        c->row_val[i] = mk_lbl_w(c->screen, 40, y - 2, 102, "--",
                                 &lv_font_montserrat_16, p->text,
                                 LV_TEXT_ALIGN_RIGHT);
        c->row_bar[i] = mk_bar(c, c->screen, 14, y + 22, 128, 4, p->track,
                               p->acc, &c->row_bar_w[i]);
    }

    /* 右列 4 行：QUEUE / GPU / TEMP / PWR */
    static const char *r_rows[4] = {"QUEUE", "GPU", "TEMP", "PWR"};
    for (int i = 0; i < 4; i++) {
        const int y = 64 + i * 50;
        mk_lbl(c->screen, 338, y + 1, r_rows[i], &lv_font_montserrat_14,
               p->dim);
        c->row_val[i + 4] = mk_lbl_w(c->screen, 364, y - 2, 102, "--",
                                     &lv_font_montserrat_16, p->text,
                                     LV_TEXT_ALIGN_RIGHT);
        c->row_bar[i + 4] = mk_bar(c, c->screen, 338, y + 22, 128, 4,
                                   p->track, p->acc2, &c->row_bar_w[i + 4]);
    }

    /* 页脚 */
    mk_rect(c, c->screen, 18, 282, 444, 1, p->border, LV_OPA_COVER);
    c->txt_lbl[0] = mk_lbl(c->screen, 20, 290, "--", &lv_font_montserrat_14,
                           p->dim);
    c->led_svc[0] = mk_led(c, c->screen, 396, 293, 7, p->dim);
    c->lbl_svc[0] = mk_lbl(c->screen, 410, 290, "STABLE",
                           &lv_font_montserrat_14, p->acc);
}

/* ============================ 主题：stealth（H03 潜行终端） ============================ */

static void build_stealth(ui_classic_t *c, const pal_t *p)
{
    /* 内嵌细线框 */
    mk_card(c, 10, 10, 460, 300, p->bg, p->border);

    /* 顶栏 */
    c->led_main = mk_led(c, c->screen, 448, 16, 7, p->acc);
    mk_lbl(c->screen, 24, 14, "STEALTH OPS", &lv_font_montserrat_16, p->acc2);
    c->lbl_model_top = mk_lbl_w(c->screen, 160, 15, 150, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_LEFT);
    c->lbl_phase_top = mk_lbl_w(c->screen, 330, 15, 108, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    mk_rect(c, c->screen, 24, 38, 432, 1, p->border, LV_OPA_COVER);

    /* 左：DECODE 大字 */
    mk_lbl(c->screen, 32, 58, "DECODE", &lv_font_montserrat_14, p->dim);
    c->lbl_big[0] = mk_lbl(c->screen, 32, 78, "--", &lv_font_montserrat_48,
                           p->text);
    mk_lbl(c->screen, 32, 134, "TOK/S", &lv_font_montserrat_14, p->dim);
    c->txt_lbl[0] = mk_lbl(c->screen, 32, 156, "AVG --",
                           &lv_font_montserrat_14, p->acc);
    mk_lbl(c->screen, 32, 186, "LOW OBSERVABILITY LINK",
           &lv_font_montserrat_14, p->dim);

    /* 右：四行遥测 */
    static const char *rows[4] = {"AVG", "PREFILL", "SLOTS", "QUEUE"};
    for (int i = 0; i < 4; i++) {
        const int y = 64 + i * 30;
        mk_lbl(c->screen, 262, y + 2, rows[i], &lv_font_montserrat_14,
               p->dim);
        c->row_val[i] = mk_lbl_w(c->screen, 330, y, 118, "--",
                                 &lv_font_montserrat_16, p->text,
                                 LV_TEXT_ALIGN_RIGHT);
        mk_rect(c, c->screen, 262, y + 24, 186, 1, p->border, LV_OPA_COVER);
    }

    /* 中段读数条 */
    mk_rect(c, c->screen, 32, 204, 416, 1, p->border, LV_OPA_COVER);
    c->txt_lbl[1] = mk_lbl(c->screen, 32, 214, "CPU --",
                           &lv_font_montserrat_14, p->text);
    c->txt_lbl[2] = mk_lbl(c->screen, 136, 214, "GPU --",
                           &lv_font_montserrat_14, p->text);
    c->txt_lbl[3] = mk_lbl(c->screen, 240, 214, "RAM --",
                           &lv_font_montserrat_14, p->text);
    c->txt_lbl[4] = mk_lbl(c->screen, 344, 214, "ACC --",
                           &lv_font_montserrat_14, p->text);

    /* 底部热管理 */
    mk_lbl(c->screen, 32, 244, "THERMAL LOAD", &lv_font_montserrat_14,
           p->dim);
    c->lbl_temp = mk_lbl(c->screen, 356, 240, "--", &lv_font_montserrat_20,
                         p->text);
    segbar_create(c, &c->row_seg[0], c->screen, 32, 268, 416, 8, p->track);

    /* 页脚：静默 + 信号方块 */
    c->txt_lbl[5] = mk_lbl(c->screen, 32, 288, "SILENT MODE • NO UPLINK",
                           &lv_font_montserrat_14, p->dim);
    c->led_svc[0] = mk_led(c, c->screen, 424, 292, 6, p->dim);
    mk_rect(c, c->screen, 438, 292, 6, 6, p->track, LV_OPA_COVER);
    mk_rect(c, c->screen, 452, 292, 6, 6, p->track, LV_OPA_COVER);
    mk_rect(c, c->screen, 466 - 10, 292, 6, 6, p->track, LV_OPA_COVER);
}

/* ============================ 主题：arcade（H04 街机计分板） ============================ */

static void build_arcade(ui_classic_t *c, const pal_t *p)
{
    /* 标题条：bg2 底 + 2px 黄色下边 */
    mk_card(c, 0, 0, 480, 34, p->bg2, p->bg2);
    mk_lbl(c->screen, 18, 9, "PLAYER 1", &lv_font_montserrat_14, p->acc2);
    c->lbl_model_top = mk_lbl_w(c->screen, 150, 9, 180, "--",
                                &lv_font_montserrat_14, p->acc,
                                LV_TEXT_ALIGN_CENTER);
    c->lbl_phase_top = mk_lbl_w(c->screen, 350, 9, 112, "--",
                                &lv_font_montserrat_14, p->acc2,
                                LV_TEXT_ALIGN_RIGHT);
    mk_rect(c, c->screen, 0, 34, 480, 2, p->acc, LV_OPA_COVER);

    /* 像素方块装饰 */
    mk_rect(c, c->screen, 18, 44, 7, 7, p->acc, LV_OPA_COVER);
    mk_rect(c, c->screen, 29, 44, 7, 7, p->acc2, LV_OPA_COVER);
    mk_rect(c, c->screen, 40, 44, 7, 7, p->track, LV_OPA_COVER);

    /* 左：CPU 大字 + 像素格 */
    mk_lbl_w(c->screen, 52, 56, 168, "CPU", &lv_font_montserrat_14, p->acc2,
             LV_TEXT_ALIGN_CENTER);
    c->lbl_big[0] = mk_lbl_w(c->screen, 52, 72, 168, "--",
                             &lv_font_montserrat_48, p->acc,
                             LV_TEXT_ALIGN_CENTER);
    mk_lbl_w(c->screen, 52, 128, 168, "PROCESSOR", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);
    segbar_create_ex(c, &c->row_seg[0], c->screen, 98, 150, 76, 12, p->track,
                     5, false);

    /* 中缝点阵柱 */
    for (int i = 0; i < 8; i++) {
        mk_rect(c, c->screen, 238, 60 + i * 13, 6, 6,
                (i % 2) ? p->track : p->acc2, LV_OPA_COVER);
    }

    /* 右：GPU 大字 + 像素格 */
    mk_lbl_w(c->screen, 260, 56, 168, "GPU", &lv_font_montserrat_14, p->acc,
             LV_TEXT_ALIGN_CENTER);
    c->lbl_big[1] = mk_lbl_w(c->screen, 260, 72, 168, "--",
                             &lv_font_montserrat_48, p->acc2,
                             LV_TEXT_ALIGN_CENTER);
    mk_lbl_w(c->screen, 260, 128, 168, "GRAPHICS", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);
    segbar_create_ex(c, &c->row_seg[1], c->screen, 306, 150, 76, 12,
                     p->track, 5, false);

    /* 中段读数条 */
    mk_card(c, 18, 192, 444, 24, p->bg, p->border);
    c->txt_lbl[0] = mk_lbl(c->screen, 26, 198, "ACC --",
                           &lv_font_montserrat_14, p->text);
    c->txt_lbl[1] = mk_lbl(c->screen, 136, 198, "T --",
                           &lv_font_montserrat_14, p->text);
    c->txt_lbl[2] = mk_lbl(c->screen, 246, 198, "P --",
                           &lv_font_montserrat_14, p->text);
    c->txt_lbl[3] = mk_lbl(c->screen, 356, 198, "VRAM --",
                           &lv_font_montserrat_14, p->text);

    /* 底部三条街机分段条：KV / MEM / HIT */
    static const char *cols[3] = {"KV", "MEM", "HIT"};
    for (int i = 0; i < 3; i++) {
        const int x = 18 + i * 152;
        mk_lbl(c->screen, x, 226, cols[i], &lv_font_montserrat_14, p->acc2);
        c->row_val[i] = mk_lbl_w(c->screen, x + 70, 224, 66, "--",
                                 &lv_font_montserrat_16, p->text,
                                 LV_TEXT_ALIGN_RIGHT);
        segbar_create_ex(c, &c->row_seg[2 + i], c->screen, x, 246, 136, 10,
                         p->track, 12, false);
    }

    /* 页脚 */
    mk_rect(c, c->screen, 18, 272, 444, 1, p->border, LV_OPA_COVER);
    c->lbl_kv_text = mk_lbl(c->screen, 22, 282, "KV --",
                            &lv_font_montserrat_14, p->dim);
    c->press_lbl = mk_lbl_w(c->screen, 300, 282, 158, "PRESS START",
                            &lv_font_montserrat_14, p->acc2,
                            LV_TEXT_ALIGN_RIGHT);
    c->txt_lbl[4] = mk_lbl(c->screen, 210, 282, "--",
                           &lv_font_montserrat_14, p->dim);
}

/* ============================ 主题：drone（H05 无人机链路） ============================ */

static void build_drone(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_card(c, 0, 0, 480, 26, p->surface, p->border);
    c->led_main = mk_led(c, c->screen, 18, 10, 7, p->acc);
    mk_lbl(c->screen, 30, 5, "DRONE LINK", &lv_font_montserrat_16, p->acc);
    c->lbl_model_top = mk_lbl_w(c->screen, 150, 6, 130, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_LEFT);
    mk_lbl(c->screen, 292, 6, "LINK", &lv_font_montserrat_14, p->dim);
    c->lbl_phase_top = mk_lbl_w(c->screen, 322, 6, 72, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    mk_lbl(c->screen, 396, 6, "SAT 12", &lv_font_montserrat_14, p->dim);
    mk_rect(c, c->screen, 18, 34, 444, 1, p->border, LV_OPA_COVER);

    /* 中央：DECODE 大字 */
    mk_lbl_w(c->screen, 140, 40, 208, "DECODE", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);
    c->lbl_big[0] = mk_lbl_w(c->screen, 140, 56, 208, "--",
                             &lv_font_montserrat_40, p->text,
                             LV_TEXT_ALIGN_CENTER);
    mk_lbl_w(c->screen, 140, 100, 208, "TOK/S", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);

    /* 人工地平仪（中心 y=178） */
    mk_rect(c, c->screen, 209, 147, 70, 1, p->acc, LV_OPA_40);
    mk_rect(c, c->screen, 209, 167, 70, 1, p->acc, LV_OPA_40);
    mk_rect(c, c->screen, 144, 177, 200, 2, p->acc, LV_OPA_80);
    mk_rect(c, c->screen, 209, 191, 70, 1, p->acc, LV_OPA_40);
    mk_rect(c, c->screen, 209, 211, 70, 1, p->acc, LV_OPA_40);
    mk_rect(c, c->screen, 214, 170, 60, 1, p->acc2, LV_OPA_COVER);
    c->txt_lbl[0] = mk_lbl_w(c->screen, 140, 240, 208, "HDG -- • PITCH --",
                             &lv_font_montserrat_14, p->dim,
                             LV_TEXT_ALIGN_CENTER);
    c->txt_lbl[1] = mk_lbl_w(c->screen, 140, 258, 120, "WP 04 • 1.8KM",
                             &lv_font_montserrat_14, p->dim,
                             LV_TEXT_ALIGN_LEFT);
    c->lbl_svc[0] = mk_lbl_w(c->screen, 250, 258, 104, "STANDBY",
                             &lv_font_montserrat_14, p->acc,
                             LV_TEXT_ALIGN_RIGHT);
    segbar_create(c, &c->row_seg[0], c->screen, 144, 276, 200, 5, p->track);

    /* 左右遥测列 */
    mk_lbl(c->screen, 14, 88, "TELEMETRY / L", &lv_font_montserrat_14,
           p->acc);
    static const char *l_rows[3] = {"GPU", "TEMP", "VRAM"};
    for (int i = 0; i < 3; i++) {
        const int y = 108 + i * 46;
        mk_lbl(c->screen, 14, y + 2, l_rows[i], &lv_font_montserrat_14,
               p->dim);
        c->row_val[i] = mk_lbl_w(c->screen, 20, y - 2, 98, "--",
                                 &lv_font_montserrat_16, p->text,
                                 LV_TEXT_ALIGN_RIGHT);
        c->row_bar[i] = mk_bar(c, c->screen, 14, y + 22, 104, 4, p->track,
                               p->acc, &c->row_bar_w[i]);
    }
    mk_lbl(c->screen, 362, 88, "TELEMETRY / R", &lv_font_montserrat_14,
           p->acc);
    static const char *r_rows[3] = {"QUEUE", "ACC", "HIT"};
    for (int i = 0; i < 3; i++) {
        const int y = 108 + i * 46;
        mk_lbl(c->screen, 362, y + 2, r_rows[i], &lv_font_montserrat_14,
               p->dim);
        c->row_val[i + 3] = mk_lbl_w(c->screen, 368, y - 2, 98, "--",
                                     &lv_font_montserrat_16, p->text,
                                     LV_TEXT_ALIGN_RIGHT);
        c->row_bar[i + 3] = mk_bar(c, c->screen, 362, y + 22, 104, 4,
                                   p->track, p->acc, &c->row_bar_w[i + 3]);
    }
    mk_rect(c, c->screen, 130, 92, 1, 162, p->border, LV_OPA_COVER);
    mk_rect(c, c->screen, 354, 92, 1, 162, p->border, LV_OPA_COVER);

    /* 页脚 */
    mk_rect(c, c->screen, 18, 290, 444, 1, p->border, LV_OPA_COVER);
    c->lbl_kv_text = mk_lbl(c->screen, 20, 298, "KV --",
                            &lv_font_montserrat_14, p->dim);
    c->txt_lbl[2] = mk_lbl(c->screen, 200, 298, "CTX --",
                           &lv_font_montserrat_14, p->dim);
    mk_lbl(c->screen, 330, 298, "AUTO", &lv_font_montserrat_14, p->acc);
}

/* ============================ 主题：scope（H06 狙击镜） ============================ */

static void build_scope(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_lbl(c->screen, 20, 10, "SNIPER SCOPE", &lv_font_montserrat_16, p->acc);
    c->led_main = mk_led(c, c->screen, 446, 14, 7, p->acc);
    c->lbl_phase_top = mk_lbl_w(c->screen, 356, 11, 76, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    mk_rect(c, c->screen, 20, 34, 440, 1, p->border, LV_OPA_COVER);

    /* 上部两角：GPU / MEM */
    mk_lbl(c->screen, 24, 44, "GPU", &lv_font_montserrat_14, p->dim);
    c->row_val[0] = mk_lbl(c->screen, 24, 62, "--", &lv_font_montserrat_20,
                           p->text);
    mk_lbl_w(c->screen, 366, 44, 90, "MEM", &lv_font_montserrat_14, p->dim,
             LV_TEXT_ALIGN_RIGHT);
    c->row_val[1] = mk_lbl_w(c->screen, 366, 62, 90, "--",
                             &lv_font_montserrat_20, p->text,
                             LV_TEXT_ALIGN_RIGHT);

    /* 中央准星环（CPU）+ 十字线 */
    c->arc_gpu = mk_arc_full(c, c->screen, 145, 62, 190, 4, p->acc,
                             p->track);
    mk_rect(c, c->screen, 150, 156, 64, 1, p->acc, LV_OPA_60);
    mk_rect(c, c->screen, 266, 156, 64, 1, p->acc, LV_OPA_60);
    mk_rect(c, c->screen, 239, 70, 1, 64, p->acc, LV_OPA_60);
    mk_rect(c, c->screen, 239, 184, 1, 64, p->acc, LV_OPA_60);
    mk_circle(c, c->screen, 206, 123, 68, p->border);
    c->lbl_big[0] = mk_lbl_w(c->screen, 140, 136, 200, "--",
                             &lv_font_montserrat_40, p->acc,
                             LV_TEXT_ALIGN_CENTER);
    mk_lbl_w(c->screen, 140, 182, 200, "CPU LOAD", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);

    /* 下部：THERMAL / PWR / KV */
    mk_lbl(c->screen, 24, 268, "THERMAL", &lv_font_montserrat_14, p->dim);
    c->lbl_temp = mk_lbl(c->screen, 100, 266, "--", &lv_font_montserrat_20,
                         p->text);
    segbar_create(c, &c->row_seg[0], c->screen, 24, 292, 180, 9, p->track);
    mk_lbl(c->screen, 240, 268, "PWR", &lv_font_montserrat_14, p->dim);
    c->lbl_power = mk_lbl(c->screen, 240, 286, "--", &lv_font_montserrat_20,
                          p->text);
    mk_lbl_w(c->screen, 390, 268, 66, "KV", &lv_font_montserrat_14, p->dim,
             LV_TEXT_ALIGN_RIGHT);
    c->row_val[4] = mk_lbl_w(c->screen, 390, 286, 66, "--",
                             &lv_font_montserrat_20, p->text,
                             LV_TEXT_ALIGN_RIGHT);

    /* 页脚 */
    c->lbl_kv_text = mk_lbl(c->screen, 24, 306, "KV --",
                            &lv_font_montserrat_14, p->dim);
    c->lbl_svc[0] = mk_lbl_w(c->screen, 360, 306, 96, "STABLE",
                             &lv_font_montserrat_14, p->acc,
                             LV_TEXT_ALIGN_RIGHT);
}

/* ============================ 主题：heatsink（H07 散热塔） ============================ */

static void build_heatsink(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_lbl(c->screen, 20, 6, "HEAT SINK", &lv_font_montserrat_16, p->acc);
    c->led_main = mk_led(c, c->screen, 404, 10, 7, p->acc);
    c->lbl_phase_top = mk_lbl_w(c->screen, 300, 7, 96, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    mk_rect(c, c->screen, 20, 32, 440, 1, p->border, LV_OPA_COVER);

    /* 左：DECODE 大字 */
    mk_lbl(c->screen, 22, 40, "DECODE", &lv_font_montserrat_14, p->dim);
    c->lbl_big[0] = mk_lbl(c->screen, 22, 58, "--", &lv_font_montserrat_48,
                           p->text);
    mk_lbl(c->screen, 126, 84, "TOK/S", &lv_font_montserrat_14, p->dim);
    c->txt_lbl[0] = mk_lbl(c->screen, 22, 114, "--", &lv_font_montserrat_14,
                           p->acc2);

    /* 右：AVG / MAX / GPU */
    c->txt_lbl[1] = mk_lbl_w(c->screen, 350, 42, 108, "AVG --",
                             &lv_font_montserrat_14, p->dim,
                             LV_TEXT_ALIGN_RIGHT);
    c->txt_lbl[2] = mk_lbl_w(c->screen, 350, 60, 108, "MAX --",
                             &lv_font_montserrat_14, p->dim,
                             LV_TEXT_ALIGN_RIGHT);
    c->txt_lbl[3] = mk_lbl_w(c->screen, 350, 78, 108, "GPU --",
                             &lv_font_montserrat_14, p->acc,
                             LV_TEXT_ALIGN_RIGHT);

    /* 三列纵向能量柱：GPU / VRAM / ACC */
    static const char *cols[3] = {"GPU", "VRAM", "ACC"};
    static const uint32_t col_colors[3] = {0, 0, 0}; /* apply 时按主题填 */
    (void)col_colors;
    for (int i = 0; i < 3; i++) {
        const int x = 30 + i * 150;
        mk_lbl(c->screen, x, 132, cols[i], &lv_font_montserrat_14, p->dim);
        c->row_val[i] = mk_lbl(c->screen, x, 150, "--",
                               &lv_font_montserrat_20, p->text);
        segbar_create_ex(c, &c->row_seg[i], c->screen, x + 23, 178, 64, 112,
                         p->track, 10, true);
        c->txt_lbl[4 + i] = mk_lbl(c->screen, x, 300, "--",
                                   &lv_font_montserrat_14, p->dim);
    }
}

/* ============================ 主题：loadout（H08 装备槽） ============================ */

static void build_loadout(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_lbl(c->screen, 20, 6, "LOADOUT", &lv_font_montserrat_16, p->acc);
    c->led_main = mk_led(c, c->screen, 404, 10, 7, p->acc);
    c->lbl_phase_top = mk_lbl_w(c->screen, 300, 7, 96, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    mk_rect(c, c->screen, 20, 32, 440, 1, p->border, LV_OPA_COVER);

    /* 四卡 2×2：CPU / GPU / MEMORY / CACHE（前缀缓存命中率） */
    static const char *nums[4] = {"01", "02", "03", "04"};
    static const char *names[4] = {"CPU", "GPU", "MEMORY", "CACHE"};
    static const char *descs[4] = {"RYZEN 9 7950X", "RTX 4070 TI",
                                   "DDR5 6000 32G", "TOKEN HIT RATE"};
    const int cx[4] = {14, 254, 14, 254};
    const int cy[4] = {40, 40, 172, 172};
    for (int i = 0; i < 4; i++) {
        const uint32_t color = (i % 2 == 0) ? p->acc : p->acc2;
        lv_obj_t *card = mk_card(c, cx[i], cy[i], 226, 126, p->surface,
                                 p->border);
        mk_corners(c, card, 226, 126, color, 8);
        mk_box_p(card, 12, 46, 36, 36, p->border);
        mk_rect_p(card, 24, 58, 12, 12, color, LV_OPA_70);
        mk_lbl(card, 58, 10, nums[i], &lv_font_montserrat_14, p->dim);
        mk_lbl(card, 78, 8, names[i], &lv_font_montserrat_16, p->text);
        mk_lbl(card, 58, 28, descs[i], &lv_font_montserrat_14, p->dim);
        c->txt_lbl[i] = mk_lbl_w(card, 58, 48, 104, "--",
                                 &lv_font_montserrat_14, p->dim,
                                 LV_TEXT_ALIGN_LEFT);
        c->row_val[i] = mk_lbl_w(card, 148, 10, 66, "--",
                                 &lv_font_montserrat_24, color,
                                 LV_TEXT_ALIGN_RIGHT);
        segbar_create(c, &c->row_seg[i], card, 58, 98, 150, 7, p->track);
    }

    /* 页脚 */
    c->txt_lbl[4] = mk_lbl(c->screen, 22, 306, "--",
                           &lv_font_montserrat_14, p->dim);
    c->lbl_svc[0] = mk_lbl_w(c->screen, 360, 306, 96, "ALL OK",
                             &lv_font_montserrat_14, p->acc,
                             LV_TEXT_ALIGN_RIGHT);
}

/* ============================ 主题：armor（H09 动力装甲） ============================ */

static void build_armor(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_lbl(c->screen, 20, 6, "POWER ARMOR", &lv_font_montserrat_16, p->acc2);
    c->led_main = mk_led(c, c->screen, 404, 10, 7, p->acc);
    c->lbl_phase_top = mk_lbl_w(c->screen, 310, 7, 86, "--",
                                &lv_font_montserrat_14, p->acc,
                                LV_TEXT_ALIGN_RIGHT);
    mk_lbl(c->screen, 20, 28, "FUSION CORE / MK-VII", &lv_font_montserrat_14,
           p->dim);

    /* 四角读数 */
    mk_lbl(c->screen, 24, 48, "PREFILL", &lv_font_montserrat_14, p->dim);
    c->row_val[0] = mk_lbl(c->screen, 24, 66, "--", &lv_font_montserrat_20,
                           p->text);
    mk_lbl_w(c->screen, 356, 48, 100, "VRAM", &lv_font_montserrat_14, p->dim,
             LV_TEXT_ALIGN_RIGHT);
    c->row_val[1] = mk_lbl_w(c->screen, 356, 66, 100, "--",
                             &lv_font_montserrat_20, p->text,
                             LV_TEXT_ALIGN_RIGHT);
    mk_lbl(c->screen, 24, 248, "TEMP", &lv_font_montserrat_14, p->dim);
    c->lbl_temp = mk_lbl(c->screen, 24, 266, "--", &lv_font_montserrat_20,
                         p->text);
    mk_lbl_w(c->screen, 356, 248, 100, "QUEUE", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_RIGHT);
    c->lbl_queue = mk_lbl_w(c->screen, 356, 266, 100, "--",
                            &lv_font_montserrat_20, p->text,
                            LV_TEXT_ALIGN_RIGHT);

    /* 能量核心：外环 GPU / 内环 ACC / 中心 DECODE */
    c->arc_gpu = mk_arc(c, c->screen, 140, 56, 200, 6, p->acc, p->track);
    c->arc_kv = mk_arc(c, c->screen, 172, 88, 136, 3, p->acc2, p->track);
    mk_lbl_w(c->screen, 190, 122, 100, "DECODE", &lv_font_montserrat_14,
             p->dim, LV_TEXT_ALIGN_CENTER);
    c->lbl_big[0] = mk_lbl_w(c->screen, 190, 140, 100, "--",
                             &lv_font_montserrat_40, p->text,
                             LV_TEXT_ALIGN_CENTER);
    mk_lbl_w(c->screen, 190, 184, 100, "TOK/S", &lv_font_montserrat_14,
             p->acc, LV_TEXT_ALIGN_CENTER);

    /* 底部：ACC 完整度条 + 页脚 */
    c->row_bar[0] = mk_bar(c, c->screen, 24, 296, 432, 4, p->track, p->acc2,
                           &c->row_bar_w[0]);
    c->lbl_kv_text = mk_lbl(c->screen, 24, 306, "KV --",
                            &lv_font_montserrat_14, p->dim);
    c->lbl_svc[0] = mk_lbl_w(c->screen, 356, 306, 100, "ACC --",
                             &lv_font_montserrat_14, p->acc2,
                             LV_TEXT_ALIGN_RIGHT);
}

/* ============================ 主题：sweep（H10 战术雷达） ============================ */

static void build_sweep(ui_classic_t *c, const pal_t *p)
{
    /* 顶栏 */
    mk_lbl(c->screen, 20, 6, "SWEEP HUD", &lv_font_montserrat_16, p->acc);
    c->led_main = mk_led(c, c->screen, 404, 10, 7, p->acc);
    c->lbl_model_top = mk_lbl_w(c->screen, 150, 7, 140, "// --",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_LEFT);
    c->lbl_phase_top = mk_lbl_w(c->screen, 310, 7, 86, "--",
                                &lv_font_montserrat_14, p->dim,
                                LV_TEXT_ALIGN_RIGHT);
    mk_lbl(c->screen, 22, 30, "RADAR / PC-01", &lv_font_montserrat_14,
           p->dim);
    mk_lbl_w(c->screen, 300, 30, 158, "GAIN 12 • R 5M",
             &lv_font_montserrat_14, p->dim, LV_TEXT_ALIGN_RIGHT);

    /* 雷达（中心 290,146） */
    mk_circle(c, c->screen, 190, 46, 200, p->border);
    mk_circle(c, c->screen, 224, 80, 132, p->track);
    mk_circle(c, c->screen, 257, 113, 66, p->track);
    mk_rect(c, c->screen, 196, 145, 188, 1, p->track, LV_OPA_COVER);
    mk_rect(c, c->screen, 289, 52, 1, 188, p->track, LV_OPA_COVER);
    mk_rect(c, c->screen, 294, 144, 82, 3, p->acc, LV_OPA_60);
    c->led_svc[0] = mk_led(c, c->screen, 286, 142, 8, p->acc);
    mk_led(c, c->screen, 250, 88, 6, p->acc);
    mk_led(c, c->screen, 330, 118, 6, p->acc2);
    mk_led(c, c->screen, 238, 188, 5, p->acc);
    mk_led(c, c->screen, 338, 198, 5, p->acc2);
    mk_led(c, c->screen, 268, 160, 5, p->acc);

    /* 四格读数：CPU / MEM / GPU / HIT */
    static const char *cells[4] = {"CPU", "MEM", "GPU", "HIT"};
    for (int i = 0; i < 4; i++) {
        const int x = 14 + i * 118;
        const uint32_t color = (i == 2) ? p->acc2 : p->acc;
        lv_obj_t *card = mk_card(c, x, 248, 106, 52, p->surface, p->border);
        mk_lbl(card, 6, 6, cells[i], &lv_font_montserrat_14, p->dim);
        c->row_val[i] = mk_lbl_w(card, 40, 4, 60, "--",
                                 &lv_font_montserrat_16, color,
                                 LV_TEXT_ALIGN_RIGHT);
        segbar_create(c, &c->row_seg[i], card, 6, 32, 94, 8, p->track);
    }

    /* 页脚 */
    c->txt_lbl[0] = mk_lbl(c->screen, 22, 306, "--",
                           &lv_font_montserrat_14, p->dim);
    c->txt_lbl[1] = mk_lbl_w(c->screen, 300, 306, 158, "--",
                             &lv_font_montserrat_14, p->acc,
                             LV_TEXT_ALIGN_RIGHT);
}

/* ============================ 构建 / 销毁 ============================ */

static int theme_from_id(const char *id)
{
    if (!id || !id[0]) return TH_INSTRUMENT;
    if (strcmp(id, "hud") == 0) return TH_HUD;
    if (strcmp(id, "mecha") == 0) return TH_MECHA;
    if (strcmp(id, "stealth") == 0) return TH_STEALTH;
    if (strcmp(id, "arcade") == 0) return TH_ARCADE;
    if (strcmp(id, "drone") == 0) return TH_DRONE;
    if (strcmp(id, "scope") == 0) return TH_SCOPE;
    if (strcmp(id, "heatsink") == 0) return TH_HEATSINK;
    if (strcmp(id, "loadout") == 0) return TH_LOADOUT;
    if (strcmp(id, "armor") == 0) return TH_ARMOR;
    if (strcmp(id, "sweep") == 0) return TH_SWEEP;
    return TH_INSTRUMENT;
}

static void build_dispatch(ui_classic_t *c, const pal_t *p)
{
    switch (c->theme) {
    case TH_HUD: build_hud(c, p); break;
    case TH_MECHA: build_mecha(c, p); break;
    case TH_STEALTH: build_stealth(c, p); break;
    case TH_ARCADE: build_arcade(c, p); break;
    case TH_DRONE: build_drone(c, p); break;
    case TH_SCOPE: build_scope(c, p); break;
    case TH_HEATSINK: build_heatsink(c, p); break;
    case TH_LOADOUT: build_loadout(c, p); break;
    case TH_ARMOR: build_armor(c, p); break;
    case TH_SWEEP: build_sweep(c, p); break;
    default: build_instrument(c, p); break;
    }
}

static void blink_cb(lv_timer_t *timer)
{
    ui_classic_t *c = lv_timer_get_user_data(timer);
    if (!c) return;
    c->blink_on = !c->blink_on;
    if (c->busy && c->led_main) {
        lv_obj_set_style_opa(c->led_main, c->blink_on ? LV_OPA_COVER
                                                      : LV_OPA_30, 0);
    } else if (c->led_main) {
        lv_obj_set_style_opa(c->led_main, LV_OPA_COVER, 0);
    }
    if (c->press_lbl) {
        lv_obj_set_style_opa(c->press_lbl,
                             c->blink_on ? LV_OPA_COVER : LV_OPA_20, 0);
    }
}

ui_classic_t *ui_classic_build(const char *theme_id, int canvas_w,
                               int canvas_h)
{
    if (canvas_w <= 0 || canvas_h <= 0) return NULL;
    ui_classic_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->theme = theme_from_id(theme_id);
    c->sx = (float)canvas_w / 480.0f;
    c->sy = (float)canvas_h / 320.0f;

    const pal_t *p = &s_pal[c->theme];

    c->screen = lv_obj_create(NULL);
    if (!c->screen) {
        free(c);
        return NULL;
    }
    lv_obj_remove_style_all(c->screen);
    lv_obj_set_style_bg_color(c->screen, lv_color_hex(p->bg), 0);
    lv_obj_set_style_bg_grad_color(c->screen, lv_color_hex(p->bg2), 0);
    lv_obj_set_style_bg_grad_dir(c->screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c->screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(c->screen, LV_OBJ_FLAG_SCROLLABLE);

    build_dispatch(c, p);

    c->blink = lv_timer_create(blink_cb, 500, c);
    ui_classic_apply(c);
    return c;
}

lv_obj_t *ui_classic_screen(ui_classic_t *c)
{
    return c ? c->screen : NULL;
}

void ui_classic_destroy(ui_classic_t *c)
{
    if (!c) return;
    if (c->blink) lv_timer_delete(c->blink);
    if (c->screen) lv_obj_del(c->screen);
    free(c);
}

/* ============================ 数值刷新 ============================ */

static void set_text_if(lv_obj_t *l, const char *text)
{
    if (l) lv_label_set_text(l, text);
}

static void set_color(lv_obj_t *l, uint32_t color)
{
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

/* 公共指标快照（一次读全，各主题按需取用） */
typedef struct {
    char model[DASH_METRIC_TEXT_LEN];
    char phase[32];
    bool online, busy;
    bool svc[3];
    double pct[N_PCT];
    bool pct_ok[N_PCT];
    double temp, power;
    bool t_ok, w_ok;
    double vram_pct;
    bool vram_ok;
    char vram_txt[24]; /* "18.2/24G" */
    char ram_txt[24];  /* "12.4/32G" */
    double decode, prefill, davg, dpeak;
    bool d_ok, p_ok, a_ok, pk_ok;
    double conc, queue;
    bool c_ok, q_ok;
    char kv_text[40];
    char ctx_txt[16]; /* "32.8K" */
    char cache_txt[24]; /* "110/110M" */
    double draft_tokens;
    bool dt_ok;
    double mem_gb;
} cvals_t;

static void read_values(cvals_t *v)
{
    m_text("llm.model", v->model, sizeof(v->model), "--");
    v->online = false;
    v->busy = false;
    m_bool("llm.online", &v->online);
    m_bool("llm.busy", &v->busy);
    m_text("llm.phase", v->phase, sizeof(v->phase),
           v->online ? (v->busy ? "BUSY" : "IDLE") : "OFFLINE");
    /* 三颗服务 LED：引擎侧三态，取不到时按熄灭处理（不沿用未初始化值） */
    v->svc[0] = v->online;
    v->svc[1] = false;
    v->svc[2] = false;
    m_bool("llm.cache_enabled", &v->svc[1]);
    m_bool("llm.drafter_loaded", &v->svc[2]);

    for (int i = 0; i < N_PCT; i++) {
        v->pct_ok[i] = pct_value(i, &v->pct[i]);
    }

    v->t_ok = m_num("host.gpu_temp_c", &v->temp);
    v->w_ok = m_num("host.gpu_power_w", &v->power);

    double vu = 0.0, vt = 0.0;
    v->vram_ok = false;
    if (m_num("host.gpu_vram_used_mb", &vu) &&
        m_num("host.gpu_vram_total_mb", &vt) && vt > 0.0) {
        v->vram_pct = vu / vt * 100.0;
        v->vram_ok = true;
        snprintf(v->vram_txt, sizeof(v->vram_txt), "%.1f/%.0fG", vu / 1024.0,
                 vt / 1024.0);
    } else {
        snprintf(v->vram_txt, sizeof(v->vram_txt), "--");
    }

    double ru = 0.0, rt = 0.0;
    v->mem_gb = 0.0;
    if (m_num("host.ram_used_mb", &ru) && m_num("host.ram_total_mb", &rt) &&
        rt > 0.0) {
        v->mem_gb = ru / 1024.0;
        snprintf(v->ram_txt, sizeof(v->ram_txt), "%.1f/%.0fG", ru / 1024.0,
                 rt / 1024.0);
    } else {
        snprintf(v->ram_txt, sizeof(v->ram_txt), "--");
    }

    v->d_ok = m_num("llm.decode_tps", &v->decode);
    v->p_ok = m_num("llm.prefill_tps", &v->prefill);
    v->a_ok = m_num("llm.decode_avg", &v->davg);
    v->pk_ok = m_num("llm.decode_peak", &v->dpeak);
    v->c_ok = m_num("llm.slots", &v->conc);
    v->q_ok = m_num("llm.requests_deferred", &v->queue);

    double used = 0.0, pool = 0.0;
    if (m_num("llm.kv_used_tokens", &used) &&
        m_num("llm.kv_pool_positions", &pool) && pool > 0.0) {
        char u[16], t[16];
        fmt_tokens(used, u, sizeof(u));
        fmt_tokens(pool, t, sizeof(t));
        snprintf(v->kv_text, sizeof(v->kv_text), "KV %s / %s", u, t);
    } else {
        snprintf(v->kv_text, sizeof(v->kv_text), "KV --");
    }

    double ctx = 0.0;
    if (m_num("llm.context_len", &ctx) && ctx > 0.0) {
        fmt_tokens(ctx, v->ctx_txt, sizeof(v->ctx_txt));
    } else {
        snprintf(v->ctx_txt, sizeof(v->ctx_txt), "--");
    }

    /* 前缀缓存：已用/上限（MB）。上限来自 /health .prompt_cache.cap_mb */
    double cbytes = 0.0, ccap = 0.0;
    if (m_num("llm.cache_bytes", &cbytes) && m_num("llm.cache_cap_mb", &ccap) &&
        ccap > 0.0) {
        snprintf(v->cache_txt, sizeof(v->cache_txt), "%.0f/%.0fM", cbytes, ccap);
    } else if (m_num("llm.cache_entries", &cbytes)) {
        snprintf(v->cache_txt, sizeof(v->cache_txt), "%.0f ENT", cbytes);
    } else {
        snprintf(v->cache_txt, sizeof(v->cache_txt), "--");
    }

    v->dt_ok = m_num("llm.draft_tokens", &v->draft_tokens);
}

/* 六个占用率：值标签 + 连续条 / 分段条（instrument 与 hud） */
static void apply_pct_common(ui_classic_t *c, const pal_t *p,
                             const cvals_t *v)
{
    char buf[16];
    for (int i = 0; i < N_PCT; i++) {
        const bool ok = v->pct_ok[i];
        const uint32_t col = ok ? pct_color(p, v->pct[i]) : p->mut;
        if (ok) snprintf(buf, sizeof(buf), "%.0f%%", v->pct[i]);
        else snprintf(buf, sizeof(buf), "--");
        set_text_if(c->pct_val[i], buf);
        set_color(c->pct_val[i], col);
        bar_set(c->pct_bar[i], c->pct_bar_w[i], ok ? v->pct[i] : 0.0, col);
        segbar_set(&c->pct_seg[i], ok ? v->pct[i] : 0.0, col);
    }
}

/* 大值 / 环通用：arc_gpu = 负载环，arc_kv = 内环 */
static void apply_arc(ui_classic_t *c, const pal_t *p, lv_obj_t *arc,
                      double val, bool ok, const char *text,
                      lv_obj_t *val_lbl, bool colored_text, bool hi)
{
    const uint32_t col = hi ? pct_color_hi(p, val) : pct_color(p, val);
    if (arc) {
        lv_arc_set_value(arc, ok ? (int)lroundf((float)val) : 0);
        lv_obj_set_style_arc_color(
            arc, lv_color_hex(ok ? col : p->mut), LV_PART_INDICATOR);
    }
    if (val_lbl) {
        set_text_if(val_lbl, text);
        set_color(val_lbl, ok ? (colored_text ? col : p->text) : p->mut);
    }
}

static void row_bar_set(ui_classic_t *c, int i, double pct, uint32_t color)
{
    bar_set(c->row_bar[i], c->row_bar_w[i], pct, color);
}

static void row_seg_set(ui_classic_t *c, int i, double pct, uint32_t color)
{
    segbar_set(&c->row_seg[i], pct, color);
}

static void set_num_lbl(lv_obj_t *l, bool ok, double v, const char *fmt,
                        uint32_t color)
{
    char buf[32];
    if (ok) snprintf(buf, sizeof(buf), fmt, v);
    else snprintf(buf, sizeof(buf), "--");
    set_text_if(l, buf);
    if (ok) set_color(l, color);
}

/* ---------- instrument / hud ---------- */

static void apply_instrument_hud(ui_classic_t *c, const pal_t *p,
                                 const cvals_t *v)
{
    char buf[64];
    const bool hud = c->theme == TH_HUD;

    apply_pct_common(c, p, v);

    apply_arc(c, p, c->arc_gpu, v->pct[0], v->pct_ok[0],
              v->pct_ok[0] ? (snprintf(buf, sizeof(buf), "%.0f%%",
                                       v->pct[0]), buf) : "--",
              c->arc_gpu_val, hud, false);

    set_num_lbl(c->lbl_temp, v->t_ok, v->temp, "%.0f",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    set_num_lbl(c->lbl_power, v->w_ok, v->power, "%.0f",
                v->w_ok ? (hud ? p->acc3 : p->text) : p->mut);

    for (int i = 0; i < 3; i++) {
        led_set(c->led_svc[i], v->svc[i], p->acc, p->mut);
        set_text_if(c->lbl_svc[i], v->svc[i] ? "ON" : "OFF");
        set_color(c->lbl_svc[i], v->svc[i] ? p->acc : p->mut);
    }

    set_text_if(c->lbl_model, v->model);
    set_num_lbl(c->lbl_conc, v->c_ok, v->conc, "%.0f", p->text);
    set_num_lbl(c->lbl_queue, v->q_ok, v->queue, "%.0f",
                v->q_ok && v->queue > 0.0 ? (hud ? p->acc2 : p->warn)
                                          : (hud ? p->acc : p->text));
    set_text_if(c->lbl_kv_text, v->kv_text);
}

/* ---------- mecha：反应堆 + 左右四行 ---------- */

static void apply_mecha(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[64];

    /* 顶部整体负载条 = GPU */
    row_seg_set(c, 0, v->pct_ok[0] ? v->pct[0] : 0.0,
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);

    /* 外环 GPU / 内环 ACC / 中心 DECODE */
    apply_arc(c, p, c->arc_gpu, v->pct[0], v->pct_ok[0], NULL, NULL, false, false);
    apply_arc(c, p, c->arc_kv, v->pct[5], v->pct_ok[5], NULL, NULL, false, true);
    set_num_lbl(c->lbl_big[0], v->d_ok, v->decode, "%.1f", p->text);

    /* 左列：PREFILL / SLOTS / VRAM / ACC */
    set_num_lbl(c->row_val[0], v->p_ok, v->prefill, "%.0f t/s",
                v->p_ok ? p->acc : p->mut);
    row_bar_set(c, 0, v->p_ok ? clamp_pct(v->prefill / 24.0) : 0.0, p->acc);
    set_num_lbl(c->row_val[1], v->c_ok, v->conc, "%.0f",
                v->c_ok ? p->acc : p->mut);
    row_bar_set(c, 1, v->c_ok ? clamp_pct(v->conc * 12.5) : 0.0, p->acc);
    set_text_if(c->row_val[2], v->vram_txt);
    set_color(c->row_val[2], v->vram_ok ? p->acc2 : p->mut);
    row_bar_set(c, 2, v->vram_ok ? v->vram_pct : 0.0, p->acc2);
    set_num_lbl(c->row_val[3], v->pct_ok[5], v->pct[5], "%.0f%%",
                v->pct_ok[5] ? pct_color_at(p, 5, v->pct[5]) : p->mut);
    row_bar_set(c, 3, v->pct_ok[5] ? v->pct[5] : 0.0, p->acc2);

    /* 右列：QUEUE / GPU / TEMP / PWR */
    set_num_lbl(c->row_val[4], v->q_ok, v->queue, "%.0f",
                v->q_ok ? p->acc2 : p->mut);
    row_bar_set(c, 4, v->q_ok ? clamp_pct(v->queue * 10.0) : 0.0, p->acc2);
    set_num_lbl(c->row_val[5], v->pct_ok[0], v->pct[0], "%.0f%%",
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);
    row_bar_set(c, 5, v->pct_ok[0] ? v->pct[0] : 0.0,
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);
    set_num_lbl(c->row_val[6], v->t_ok, v->temp, "%.0f°",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    row_bar_set(c, 6, v->t_ok ? clamp_pct(v->temp) : 0.0,
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    set_num_lbl(c->row_val[7], v->w_ok, v->power, "%.0fW",
                v->w_ok ? p->acc3 : p->mut);
    row_bar_set(c, 7, v->w_ok ? clamp_pct(v->power / 4.0) : 0.0, p->acc3);

    /* 页脚 */
    if (v->t_ok && v->w_ok)
        snprintf(buf, sizeof(buf), "CORE %.0fC • PWR %.0fW • COOL %.0f%%",
                 v->temp, v->power, clamp_pct(100.0 - v->temp));
    else
        snprintf(buf, sizeof(buf), "CORE -- • PWR --");
    set_text_if(c->txt_lbl[0], buf);
    led_set(c->led_svc[0], v->busy, p->acc, p->mut);
    set_text_if(c->lbl_svc[0],
                v->online ? (v->busy ? "STABLE" : "IDLE") : "OFFLINE");
    set_color(c->lbl_svc[0],
              v->online ? (v->busy ? p->acc : p->dim) : p->err);
    led_set(c->led_svc[1], v->svc[1], p->acc2, p->mut);
    led_set(c->led_svc[2], v->svc[2], p->acc2, p->mut);
}

/* ---------- stealth：极简细线 ---------- */

static void apply_stealth(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[32];

    set_num_lbl(c->lbl_big[0], v->d_ok, v->decode, "%.1f", p->text);
    if (v->a_ok) snprintf(buf, sizeof(buf), "AVG %.1f", v->davg);
    else snprintf(buf, sizeof(buf), "AVG --");
    set_text_if(c->txt_lbl[0], buf);

    /* 右侧四行：AVG / PREFILL / SLOTS / QUEUE */
    set_num_lbl(c->row_val[0], v->a_ok, v->davg, "%.1f", p->text);
    set_num_lbl(c->row_val[1], v->p_ok, v->prefill, "%.0f", p->text);
    set_num_lbl(c->row_val[2], v->c_ok, v->conc, "%.0f", p->text);
    set_num_lbl(c->row_val[3], v->q_ok, v->queue, "%.0f", p->text);

    /* 中段：CPU / GPU / RAM / ACC */
    if (v->pct_ok[1]) snprintf(buf, sizeof(buf), "CPU %.0f%%", v->pct[1]);
    else snprintf(buf, sizeof(buf), "CPU --");
    set_text_if(c->txt_lbl[1], buf);
    if (v->pct_ok[0]) snprintf(buf, sizeof(buf), "GPU %.0f%%", v->pct[0]);
    else snprintf(buf, sizeof(buf), "GPU --");
    set_text_if(c->txt_lbl[2], buf);
    if (v->mem_gb > 0.0) snprintf(buf, sizeof(buf), "RAM %.1fG", v->mem_gb);
    else snprintf(buf, sizeof(buf), "RAM --");
    set_text_if(c->txt_lbl[3], buf);
    if (v->pct_ok[5]) snprintf(buf, sizeof(buf), "ACC %.0f%%", v->pct[5]);
    else snprintf(buf, sizeof(buf), "ACC --");
    set_text_if(c->txt_lbl[4], buf);

    /* 底部热管理 */
    set_num_lbl(c->lbl_temp, v->t_ok, v->temp, "%.0f",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    row_seg_set(c, 0, v->t_ok ? v->temp : 0.0,
                v->t_ok ? temp_color(p, v->temp) : p->mut);

    led_set(c->led_svc[0], v->online, p->acc, p->track);
}

/* ---------- arcade：街机计分板 ---------- */

static void apply_arcade(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[32];

    /* 双大字：CPU / GPU + 像素格 */
    set_num_lbl(c->lbl_big[0], v->pct_ok[1], v->pct[1], "%.0f%%",
                v->pct_ok[1] ? pct_color(p, v->pct[1]) : p->mut);
    row_seg_set(c, 0, v->pct_ok[1] ? v->pct[1] : 0.0,
                v->pct_ok[1] ? pct_color(p, v->pct[1]) : p->mut);
    set_num_lbl(c->lbl_big[1], v->pct_ok[0], v->pct[0], "%.0f%%",
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);
    row_seg_set(c, 1, v->pct_ok[0] ? v->pct[0] : 0.0,
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);

    /* 中段读数 */
    set_num_lbl(c->txt_lbl[0], v->pct_ok[5], v->pct[5], "ACC %.0f%%",
                v->pct_ok[5] ? pct_color_at(p, 5, v->pct[5]) : p->mut);
    set_num_lbl(c->txt_lbl[1], v->t_ok, v->temp, "T %.0fC",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    set_num_lbl(c->txt_lbl[2], v->w_ok, v->power, "P %.0fW",
                v->w_ok ? p->text : p->mut);
    set_num_lbl(c->txt_lbl[3], v->vram_ok, v->vram_pct, "VRAM %.0f%%",
                v->vram_ok ? p->text : p->mut);

    /* 底部三条：KV / MEM / HIT */
    const int idx[3] = {2, 3, 4};
    for (int i = 0; i < 3; i++) {
        const int pi = idx[i];
        set_num_lbl(c->row_val[i], v->pct_ok[pi], v->pct[pi], "%.0f%%",
                    v->pct_ok[pi] ? pct_color(p, v->pct[pi]) : p->mut);
        row_seg_set(c, 2 + i, v->pct_ok[pi] ? v->pct[pi] : 0.0,
                    v->pct_ok[pi] ? pct_color(p, v->pct[pi]) : p->mut);
    }

    set_text_if(c->lbl_kv_text, v->kv_text);
    if (v->t_ok && v->w_ok)
        snprintf(buf, sizeof(buf), "%.0fC • %.0fW", v->temp, v->power);
    else
        snprintf(buf, sizeof(buf), "--");
    set_text_if(c->txt_lbl[4], buf);
}

/* ---------- drone：无人机链路 ---------- */

static void apply_drone(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[40];

    set_num_lbl(c->lbl_big[0], v->d_ok, v->decode, "%.1f", p->text);

    /* 左列：GPU / TEMP / VRAM */
    set_num_lbl(c->row_val[0], v->pct_ok[0], v->pct[0], "%.0f%%",
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);
    row_bar_set(c, 0, v->pct_ok[0] ? v->pct[0] : 0.0,
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);
    set_num_lbl(c->row_val[1], v->t_ok, v->temp, "%.0f°",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    row_bar_set(c, 1, v->t_ok ? clamp_pct(v->temp) : 0.0,
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    set_num_lbl(c->row_val[2], v->vram_ok, v->vram_pct, "%.0f%%",
                v->vram_ok ? p->acc : p->mut);
    row_bar_set(c, 2, v->vram_ok ? v->vram_pct : 0.0, p->acc);

    /* 右列：QUEUE / ACC / HIT */
    set_num_lbl(c->row_val[3], v->q_ok, v->queue, "%.0f",
                v->q_ok ? p->acc2 : p->mut);
    row_bar_set(c, 3, v->q_ok ? clamp_pct(v->queue * 10.0) : 0.0, p->acc2);
    set_num_lbl(c->row_val[4], v->pct_ok[5], v->pct[5], "%.0f%%",
                v->pct_ok[5] ? pct_color_at(p, 5, v->pct[5]) : p->mut);
    row_bar_set(c, 4, v->pct_ok[5] ? v->pct[5] : 0.0,
                v->pct_ok[5] ? pct_color_at(p, 5, v->pct[5]) : p->mut);
    set_num_lbl(c->row_val[5], v->pct_ok[4], v->pct[4], "%.0f%%",
                v->pct_ok[4] ? pct_color_at(p, 4, v->pct[4]) : p->mut);
    row_bar_set(c, 5, v->pct_ok[4] ? v->pct[4] : 0.0,
                v->pct_ok[4] ? pct_color_at(p, 4, v->pct[4]) : p->mut);

    /* 底部弹药条 = KV */
    row_seg_set(c, 0, v->pct_ok[2] ? v->pct[2] : 0.0,
                v->pct_ok[2] ? pct_color(p, v->pct[2]) : p->mut);

    snprintf(buf, sizeof(buf), "HDG 274 • PITCH +1.2");
    set_text_if(c->txt_lbl[0], buf);
    snprintf(buf, sizeof(buf), "WP 04 • 1.8KM");
    set_text_if(c->txt_lbl[1], buf);
    set_text_if(c->lbl_svc[0], v->busy ? "AUTO HOLD" : "STANDBY");
    set_color(c->lbl_svc[0], v->busy ? p->acc : p->dim);
    set_text_if(c->lbl_kv_text, v->kv_text);
    snprintf(buf, sizeof(buf), "CTX %s", v->ctx_txt);
    set_text_if(c->txt_lbl[2], buf);
}

/* ---------- scope：狙击镜 ---------- */

static void apply_scope(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[32];

    /* 准星环 = CPU */
    apply_arc(c, p, c->arc_gpu, v->pct[1], v->pct_ok[1],
              v->pct_ok[1] ? (snprintf(buf, sizeof(buf), "%.0f%%",
                                       v->pct[1]), buf) : "--",
              c->lbl_big[0], true, false);

    /* 四角：GPU / MEM / TEMP / PWR + KV */
    set_num_lbl(c->row_val[0], v->pct_ok[0], v->pct[0], "%.0f%%",
                v->pct_ok[0] ? pct_color(p, v->pct[0]) : p->mut);
    set_text_if(c->row_val[1], v->ram_txt);
    set_color(c->row_val[1], v->mem_gb > 0.0 ? p->text : p->mut);
    set_num_lbl(c->lbl_temp, v->t_ok, v->temp, "%.0f",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    row_seg_set(c, 0, v->t_ok ? v->temp : 0.0,
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    set_num_lbl(c->lbl_power, v->w_ok, v->power, "%.0f",
                v->w_ok ? p->text : p->mut);
    set_num_lbl(c->row_val[4], v->pct_ok[2], v->pct[2], "%.0f%%",
                v->pct_ok[2] ? pct_color(p, v->pct[2]) : p->mut);

    set_text_if(c->lbl_kv_text, v->kv_text);
    set_text_if(c->lbl_svc[0],
                v->online ? (v->busy ? "STABLE" : "IDLE") : "OFFLINE");
    set_color(c->lbl_svc[0],
              v->online ? (v->busy ? p->acc : p->dim) : p->err);
}

/* ---------- heatsink：散热塔三柱 ---------- */

static void apply_heatsink(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[DASH_METRIC_TEXT_LEN + 24];

    set_num_lbl(c->lbl_big[0], v->d_ok, v->decode, "%.1f", p->text);
    if (v->c_ok && v->model[0])
        snprintf(buf, sizeof(buf), "%s • SEQ %.0f", v->model, v->conc);
    else
        snprintf(buf, sizeof(buf), "--");
    set_text_if(c->txt_lbl[0], buf);

    set_num_lbl(c->txt_lbl[1], v->a_ok, v->davg, "AVG %.1f", p->dim);
    set_num_lbl(c->txt_lbl[2], v->pk_ok, v->dpeak, "MAX %.1f", p->dim);
    set_num_lbl(c->txt_lbl[3], v->pct_ok[0], v->pct[0], "GPU %.0f%%",
                p->acc);

    /* 三柱：GPU / VRAM / ACC */
    const uint32_t cols[3] = {p->acc, p->acc2, p->acc};
    const int pct_idx[3] = {0, -1, 5}; /* VRAM 用 vram_pct */
    const double vals[3] = {v->pct[0], v->vram_pct, v->pct[5]};
    const bool oks[3] = {v->pct_ok[0], v->vram_ok, v->pct_ok[5]};
    for (int i = 0; i < 3; i++) {
        (void)pct_idx;
        set_num_lbl(c->row_val[i], oks[i], vals[i], "%.0f%%",
                    oks[i] ? cols[i] : p->mut);
        row_seg_set(c, i, oks[i] ? vals[i] : 0.0, oks[i] ? cols[i] : p->mut);
    }

    /* 柱底说明 */
    if (v->w_ok && v->t_ok)
        snprintf(buf, sizeof(buf), "P %.0fW • %.0fC", v->power, v->temp);
    else
        snprintf(buf, sizeof(buf), "--");
    set_text_if(c->txt_lbl[4], buf);
    set_text_if(c->txt_lbl[5], v->ram_txt);
    /* 第三柱是 ACC（草稿接受率），柱底配草稿 token 总数，语义对齐 */
    if (v->dt_ok) {
        char tk[16];
        fmt_tokens(v->draft_tokens, tk, sizeof(tk));
        snprintf(buf, sizeof(buf), "DRAFT %s", tk);
    } else {
        snprintf(buf, sizeof(buf), "DRAFT --");
    }
    set_text_if(c->txt_lbl[6], buf);
}

/* ---------- loadout：装备槽四卡 ---------- */

static void apply_loadout(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[40];
    const int pct_idx[4] = {1, 0, 3, 4}; /* CPU GPU MEM HIT */

    for (int i = 0; i < 4; i++) {
        const int pi = pct_idx[i];
        set_num_lbl(c->row_val[i], v->pct_ok[pi], v->pct[pi], "%.0f%%",
                    v->pct_ok[pi] ? pct_color(p, v->pct[pi]) : p->mut);
        row_seg_set(c, i, v->pct_ok[pi] ? v->pct[pi] : 0.0,
                    v->pct_ok[pi] ? pct_color(p, v->pct[pi]) : p->mut);
    }

    /* 卡片第二行说明 */
    if (v->t_ok) snprintf(buf, sizeof(buf), "TEMP %.0fC", v->temp);
    else snprintf(buf, sizeof(buf), "TEMP --");
    set_text_if(c->txt_lbl[0], buf);
    if (v->w_ok && v->t_ok)
        snprintf(buf, sizeof(buf), "%.0fW • %.0fC", v->power, v->temp);
    else
        snprintf(buf, sizeof(buf), "--");
    set_text_if(c->txt_lbl[1], buf);
    set_text_if(c->txt_lbl[2], v->ram_txt);
    set_text_if(c->txt_lbl[3], v->cache_txt);

    if (v->t_ok && v->w_ok)
        snprintf(buf, sizeof(buf), "TEMP %.0fC • PWR %.0fW", v->temp,
                 v->power);
    else
        snprintf(buf, sizeof(buf), "TEMP -- • PWR --");
    set_text_if(c->txt_lbl[4], buf);
    set_text_if(c->lbl_svc[0], v->online ? "ALL OK" : "OFFLINE");
    set_color(c->lbl_svc[0], v->online ? p->acc : p->err);
}

/* ---------- armor：动力装甲 ---------- */

static void apply_armor(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[32];

    /* 四角 */
    set_num_lbl(c->row_val[0], v->p_ok, v->prefill, "%.0f t/s",
                v->p_ok ? p->acc3 : p->mut);
    set_text_if(c->row_val[1], v->vram_txt);
    set_color(c->row_val[1], v->vram_ok ? p->text : p->mut);
    set_num_lbl(c->lbl_temp, v->t_ok, v->temp, "%.0fC",
                v->t_ok ? temp_color(p, v->temp) : p->mut);
    set_num_lbl(c->lbl_queue, v->q_ok, v->queue, "%.0f",
                v->q_ok ? p->text : p->mut);

    /* 核心：外环 GPU / 内环 ACC / 中心 DECODE */
    apply_arc(c, p, c->arc_gpu, v->pct[0], v->pct_ok[0], NULL, NULL, false, false);
    apply_arc(c, p, c->arc_kv, v->pct[5], v->pct_ok[5], NULL, NULL, false, true);
    set_num_lbl(c->lbl_big[0], v->d_ok, v->decode, "%.1f", p->text);

    /* 底部 ACC 完整度条 */
    row_bar_set(c, 0, v->pct_ok[5] ? v->pct[5] : 0.0,
                v->pct_ok[5] ? pct_color_at(p, 5, v->pct[5]) : p->mut);
    set_text_if(c->lbl_kv_text, v->kv_text);
    set_num_lbl(c->lbl_svc[0], v->pct_ok[5], v->pct[5], "ACC %.0f%%",
                v->pct_ok[5] ? p->acc2 : p->mut);
    (void)buf;
}

/* ---------- sweep：战术雷达 ---------- */

static void apply_sweep(ui_classic_t *c, const pal_t *p, const cvals_t *v)
{
    char buf[48];
    const int pct_idx[4] = {1, 3, 0, 4}; /* CPU MEM GPU HIT */

    for (int i = 0; i < 4; i++) {
        const int pi = pct_idx[i];
        set_num_lbl(c->row_val[i], v->pct_ok[pi], v->pct[pi], "%.0f%%",
                    v->pct_ok[pi] ? pct_color(p, v->pct[pi]) : p->mut);
        row_seg_set(c, i, v->pct_ok[pi] ? v->pct[pi] : 0.0,
                    v->pct_ok[pi] ? pct_color(p, v->pct[pi]) : p->mut);
    }

    if (v->t_ok && v->w_ok) {
        char acc[8];
        if (v->pct_ok[5]) snprintf(acc, sizeof(acc), "%.0f%%", v->pct[5]);
        else snprintf(acc, sizeof(acc), "--");
        snprintf(buf, sizeof(buf), "T %.0fC • P %.0fW • ACC %s", v->temp,
                 v->power, acc);
    } else
        snprintf(buf, sizeof(buf), "T -- • P --");
    set_text_if(c->txt_lbl[0], buf);
    if (v->c_ok)
        snprintf(buf, sizeof(buf), "SEQ %.0f • Q %.0f", v->conc, v->queue);
    else
        snprintf(buf, sizeof(buf), "SEQ --");
    set_text_if(c->txt_lbl[1], buf);
    led_set(c->led_svc[0], v->online, p->acc, p->mut);
}

/* ---------- 总入口 ---------- */

void ui_classic_apply(ui_classic_t *c)
{
    if (!c) return;
    const pal_t *p = &s_pal[c->theme];
    cvals_t v;
    char buf[DASH_METRIC_TEXT_LEN + 8];

    read_values(&v);

    /* 顶栏：主 LED / 模型 / 阶段 */
    bool prefix = c->theme == TH_HUD || c->theme == TH_MECHA ||
                  c->theme == TH_DRONE || c->theme == TH_SWEEP;
    if (prefix) snprintf(buf, sizeof(buf), "// %s", v.model);
    else snprintf(buf, sizeof(buf), "%s", v.model);
    set_text_if(c->lbl_model_top, buf);
    set_text_if(c->lbl_phase_top, v.phase);
    set_color(c->lbl_phase_top,
              v.busy ? p->acc2 : (v.online ? p->dim : p->err));
    led_set(c->led_main, v.online, p->acc, p->mut);
    c->busy = v.busy;

    switch (c->theme) {
    case TH_HUD:
    case TH_INSTRUMENT: apply_instrument_hud(c, p, &v); break;
    case TH_MECHA: apply_mecha(c, p, &v); break;
    case TH_STEALTH: apply_stealth(c, p, &v); break;
    case TH_ARCADE: apply_arcade(c, p, &v); break;
    case TH_DRONE: apply_drone(c, p, &v); break;
    case TH_SCOPE: apply_scope(c, p, &v); break;
    case TH_HEATSINK: apply_heatsink(c, p, &v); break;
    case TH_LOADOUT: apply_loadout(c, p, &v); break;
    case TH_ARMOR: apply_armor(c, p, &v); break;
    case TH_SWEEP: apply_sweep(c, p, &v); break;
    default: break;
    }
}
