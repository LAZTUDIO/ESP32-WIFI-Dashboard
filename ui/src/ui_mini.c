/**
 * 小组件 2.0 实现：10 种新组件类型的 LVGL 渲染与刷新。
 *
 * 设计约束（与图鉴一致）：
 *  - 只用 LVGL 图元：标签、圆弧、条、图表、圆角矩形、绘制事件补线；
 *  - 无滤镜/毛玻璃/3D；动画仅透明度与数值；
 *  - 柱状图/迷你曲线用"一个透明覆盖层 + 绘制事件"画，不新增逐柱对象，
 *    控制 obj 数量与刷新开销。
 */
#include "ui_mini.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_dynamic.h"

#define UI_MINI_PI 3.14159265f
#define UI_MINI_ANIM_MAX 24

struct ui_mini {
    char type[24];
    ui_mini_spec_t spec;
    int gw, gh;
    int body_w, body_h;
    lv_obj_t *parent;

    ui_mini_bind_t binds[UI_MINI_MAX_BINDS];
    int n_binds;
    ui_mini_row_t rows[UI_MINI_MAX_ROWS];
    int n_rows;

    /* 通用子对象 */
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *sub;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *arc;
    lv_obj_t *dots[UI_MINI_MAX_ROWS];
    lv_obj_t *labels[UI_MINI_MAX_ROWS];
    lv_obj_t *values[UI_MINI_MAX_ROWS];
    lv_obj_t *chart;
    lv_chart_series_t *ser1;
    lv_chart_series_t *ser2;
    lv_obj_t *overlay;
    lv_obj_t *trend;

    /* 绘制几何缓存 */
    int g_d, g_cx, g_cy;   /* gauge_ring */
    double g_ratio;        /* gauge_ring 指针角度（仪表皮肤） */
    double vbar_ratio;     /* vbar_gauge 液面线（仪表皮肤） */
    double bar_max;        /* bar_strip / vbar_gauge 量程 */
    int mb_top, mb_bottom; /* multi_bars 预留 */
    int sp_label_h;        /* spark_row 底部标签高 */
    int track_w, track_h;  /* 条/柱几何（构建时缓存，apply 不再查询 coords） */
    int col_w[UI_MINI_MAX_ROWS]; /* spark_row 列宽缓存 */
    int value_w;           /* text_badge 值宽缓存 */

    /* 历史 */
    float hist[UI_MINI_HIST];
    int hist_n, hist_next;
    float hist2[UI_MINI_HIST];
    int hist2_n, hist2_next;
    float rowhist[UI_MINI_MAX_ROWS][UI_MINI_ROW_HIST];
    int rowhist_n[UI_MINI_MAX_ROWS];
    int rowhist_next[UI_MINI_MAX_ROWS];

    /* 动画 */
    void (*tick)(ui_mini_t *m);
    bool led_on;
};

/* ---------------- 基础helpers ---------------- */

static const lv_font_t *f_small(void) { return ui_dynamic_font_small(); }
static const lv_font_t *f_medium(void) { return ui_dynamic_font_medium(); }
static const lv_font_t *f_large(void) { return ui_dynamic_font_large(); }

static lv_obj_t *mlabel(lv_obj_t *parent, const char *text,
                        const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *old = lv_label_get_text(label);
    if (old && strcmp(old, text) == 0) return;
    lv_label_set_text(label, text);
}

static bool text_is_ascii(const char *text)
{
    for (; text && *text; text++) {
        if ((unsigned char)*text >= 0x80) return false;
    }
    return true;
}

static void fmt_metric(char *out, size_t cap, const dash_metric_t *m,
                       int precision)
{
    if (!m || !m->available) {
        snprintf(out, cap, "--");
    } else if (m->type == DASH_METRIC_NUMBER) {
        if (precision < 0) precision = 0;
        if (precision > 3) precision = 3;
        snprintf(out, cap, "%.*f", precision, m->number);
    } else if (m->type == DASH_METRIC_BOOL) {
        snprintf(out, cap, "%s", m->boolean ? "是" : "否");
    } else {
        /* 文本指标可能长于调用方缓冲：显式限长，避免静默截断告警 */
        const char *txt = m->text[0] ? m->text : "--";
        snprintf(out, cap, "%.*s", (int)(cap > 0 ? cap - 1 : 0), txt);
    }
}

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

/** 自适应量程：历史波动相对量级太小时把范围撑开。
 *  否则内存 10944MB 上的 1MB 抖动也会被拉满整张图，
 *  画成一串从底到顶的竖直尖刺。 */
static void auto_range(float v, float *lo, float *hi)
{
    float span = *hi - *lo;
    float min_span = fabsf(v) * 0.03f;
    if (min_span < 2.0f) min_span = 2.0f;
    if (span < min_span) {
        float mid = (*lo + *hi) * 0.5f;
        *lo = mid - min_span * 0.5f;
        *hi = mid + min_span * 0.5f;
    } else {
        /* 上下各留 20% 余量，曲线贴着图框上下沿会显得像故障 */
        float pad = span * 0.2f;
        if (pad < 0.5f) pad = 0.5f;
        *lo -= pad;
        *hi += pad;
    }
    if (*lo < 0.0f && v >= 0.0f) *lo = 0.0f;
}

/** 阈值着色：越危险线用 err，越警告线用 warn，否则主色。 */
static uint32_t mini_value_color(double v, const ui_mini_spec_t *sp)
{
    const ui_theme_t *th = ui_theme();
    if (sp->danger_above > 0 && v >= sp->danger_above) return th->err;
    if (sp->warn_above > 0 && v >= sp->warn_above) return th->warn;
    return th->skin == UI_SKIN_VINTAGE ? th->accent_2 : th->accent;
}

/** 行内标签：卡片带标题栏时用指标名，避免与卡片标题重复；
 *  2 行内联形态没有标题栏，仍用组件标题。 */
static const char *mini_inline_title(const ui_mini_t *m)
{
    if (m->gh >= 3 && m->binds[0].label[0]) return m->binds[0].label;
    return m->spec.title;
}

static uint32_t mini_track_color(void)
{
    const ui_theme_t *th = ui_theme();
    if (th->skin == UI_SKIN_MINIMAL) return th->border;
    /* 复古机械：轨道要比卡片底色更深，才像铣出来的凹槽；
     * surface_2 只比 surface 深一点，远看会和底板糊在一起。 */
    if (th->skin == UI_SKIN_VINTAGE) return th->bg;
    return th->surface_2;
}

/** 量程：显式 max > %/°C 单位 100 > 软缩放(v/(v+1))。 */
static double mini_max_of(const ui_mini_t *m, const dash_metric_t *metric,
                          const char *unit)
{
    if (m->spec.max_value > 0) return m->spec.max_value;
    if (unit && (strcmp(unit, "%") == 0 || strstr(unit, "°C") != NULL)) {
        return 100.0;
    }
    return 0.0;
}

static double mini_ratio(const ui_mini_t *m, const dash_metric_t *metric,
                         const char *unit)
{
    if (!metric || !metric->available || metric->type != DASH_METRIC_NUMBER) {
        return 0.0;
    }
    double v = metric->number;
    if (v < 0) v = 0;
    double maxv = mini_max_of(m, metric, unit);
    if (maxv > 0) return clamp01(v / maxv);
    return clamp01(v / (v + 1.0));
}

static void draw_seg(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                     lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static void draw_rect(lv_layer_t *layer, int x, int y, int w, int h,
                      lv_color_t color, int radius, lv_opa_t opa)
{
    if (w <= 0 || h <= 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &d, &a);
}

/** 仪表皮肤：复古机械主题才有表圈/游丝刻度/液面线等机械元素。 */
static bool mini_vintage(void)
{
    return ui_theme()->skin == UI_SKIN_VINTAGE;
}

/** 1px 圆环（表圈内环）：只画描边，不填背景。 */
static void draw_ring(lv_layer_t *layer, int cx, int cy, int r,
                      lv_color_t color, int width, lv_opa_t opa)
{
    if (r <= 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = color;
    d.border_width = width;
    d.border_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(layer, &d, &a);
}

/* ---------------- 共享动画时钟 ---------------- */

static ui_mini_t *s_anim[UI_MINI_ANIM_MAX];
static int s_anim_n;
static lv_timer_t *s_anim_timer;
static uint32_t s_phase;

static void anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_phase += 50;
    for (int i = 0; i < s_anim_n; i++) {
        if (s_anim[i] && s_anim[i]->tick) s_anim[i]->tick(s_anim[i]);
    }
}

static void anim_register(ui_mini_t *m)
{
    if (!m->tick || s_anim_n >= UI_MINI_ANIM_MAX) return;
    s_anim[s_anim_n++] = m;
    if (!s_anim_timer) s_anim_timer = lv_timer_create(anim_timer_cb, 50, NULL);
}

static void anim_unregister(ui_mini_t *m)
{
    for (int i = 0; i < s_anim_n; i++) {
        if (s_anim[i] == m) {
            s_anim[i] = s_anim[s_anim_n - 1];
            s_anim_n--;
            break;
        }
    }
    if (s_anim_n == 0 && s_anim_timer) {
        lv_timer_delete(s_anim_timer);
        s_anim_timer = NULL;
    }
}

uint32_t ui_mini_phase(void) { return s_phase % 1000; }

/* ---------------- 绘制事件 ---------------- */

/* gauge_ring 刻度环 */
static void gauge_tick_cb(lv_event_t *e)
{
    ui_mini_t *m = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!m || !layer) return;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const ui_theme_t *th = ui_theme();
    lv_color_t c = lv_color_hex(th->border);
    int cx = a.x1 + m->g_cx;
    int cy = a.y1 + m->g_cy;
    int r_out = m->g_d / 2 + 3;

    if (mini_vintage()) {
        /* 机械表：270° 内 46 格游丝刻度（6° 一格，5 格加长），
         * 外侧一圈内表圈，再用黄铜指针指示当前值。 */
        for (int i = 0; i <= 45; i++) {
            float ang = (135.0f + 270.0f * i / 45.0f) * UI_MINI_PI / 180.0f;
            bool major = (i % 5 == 0);
            int r_in = r_out - (major ? 6 : 3);
            int x1 = cx + (int)(cosf(ang) * r_in);
            int y1 = cy + (int)(sinf(ang) * r_in);
            int x2 = cx + (int)(cosf(ang) * r_out);
            int y2 = cy + (int)(sinf(ang) * r_out);
            draw_seg(layer, x1, y1, x2, y2,
                     major ? lv_color_hex(th->accent_2) : c, major ? 2 : 1,
                     major ? LV_OPA_70 : LV_OPA_40);
        }
        if (m->g_d >= 56) {
            draw_ring(layer, cx, cy, r_out - 9, c, 1, LV_OPA_30);
        }
        /* 指针：从盘心 45% 半径指到 82%，2px 黄铜线 + 中心轴帽，
         * 1px 在 320 屏上经抗锯齿后过暗，读不出来。 */
        if (m->g_ratio > 0.005) {
            float ang =
                (135.0f + 270.0f * (float)m->g_ratio) * UI_MINI_PI / 180.0f;
            int r1 = m->g_d * 45 / 200;
            int r2 = m->g_d * 82 / 200;
            lv_color_t needle = lv_color_hex(th->accent_2);
            draw_seg(layer, cx + (int)(cosf(ang) * r1),
                     cy + (int)(sinf(ang) * r1),
                     cx + (int)(cosf(ang) * r2),
                     cy + (int)(sinf(ang) * r2), needle, 2, LV_OPA_90);
            int tip_x = cx + (int)(cosf(ang) * (r2 + 1));
            int tip_y = cy + (int)(sinf(ang) * (r2 + 1));
            draw_rect(layer, tip_x - 1, tip_y - 1, 2, 2, needle,
                      LV_RADIUS_CIRCLE, LV_OPA_COVER);
        }
        /* 中心轴帽：机械表的转轴 */
        if (m->g_d >= 40) {
            draw_rect(layer, cx - 1, cy - 1, 3, 3,
                      lv_color_hex(th->accent_2), LV_RADIUS_CIRCLE,
                      LV_OPA_COVER);
        }
        return;
    }

    for (int i = 0; i <= 12; i++) {
        float ang = (135.0f + 270.0f * i / 12.0f) * UI_MINI_PI / 180.0f;
        bool major = (i % 3 == 0);
        int r_in = r_out - (major ? 5 : 3);
        int x1 = cx + (int)(cosf(ang) * r_in);
        int y1 = cy + (int)(sinf(ang) * r_in);
        int x2 = cx + (int)(cosf(ang) * r_out);
        int y2 = cy + (int)(sinf(ang) * r_out);
        draw_seg(layer, x1, y1, x2, y2, c, major ? 2 : 1,
                 major ? LV_OPA_60 : LV_OPA_30);
    }
}

/* 水平条阈值刻度（bar_strip） */
static void bar_tick_cb(lv_event_t *e)
{
    ui_mini_t *m = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!m || !layer) return;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int w = a.x2 - a.x1 + 1;
    int h = a.y2 - a.y1 + 1;
    double maxv = m->bar_max > 0 ? m->bar_max : 100.0;
    const ui_theme_t *th = ui_theme();
    if (mini_vintage()) {
        /* 机械刻度条：10 等分刻线（长 3px / 短 2px 交替）+ 内部细分隔线 */
        lv_color_t line = lv_color_hex(th->border);
        for (int i = 0; i <= 10; i++) {
            int x = a.x1 + w * i / 10;
            bool major = (i % 2 == 0);
            draw_seg(layer, x, a.y1, x, a.y1 + (major ? 3 : 2),
                     major ? lv_color_hex(th->accent_2) : line, 1,
                     major ? LV_OPA_60 : LV_OPA_40);
            if (i > 0 && i < 10) {
                draw_seg(layer, x, a.y1 + 3, x, a.y2, line, 1, LV_OPA_20);
            }
        }
    }
    if (m->spec.warn_above > 0) {
        int x = a.x1 + (int)((m->spec.warn_above / maxv) * w);
        draw_seg(layer, x, a.y1, x, a.y2, lv_color_hex(th->warn), 1, LV_OPA_80);
    }
    if (m->spec.danger_above > 0) {
        int x = a.x1 + (int)((m->spec.danger_above / maxv) * w);
        draw_seg(layer, x, a.y1, x, a.y2, lv_color_hex(th->err), 1, LV_OPA_80);
    }
    (void)h;
}

/* 垂直柱刻度（vbar_gauge）：警告/危险横线 + 左侧小刻度 */
static void vbar_tick_cb(lv_event_t *e)
{
    ui_mini_t *m = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!m || !layer) return;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const ui_theme_t *th = ui_theme();
    int w = a.x2 - a.x1 + 1;
    int h = a.y2 - a.y1 + 1;
    double maxv = m->bar_max > 0 ? m->bar_max : 100.0;
    if (m->spec.warn_above > 0) {
        int y = a.y2 - (int)((m->spec.warn_above / maxv) * h);
        draw_seg(layer, a.x1, y, a.x2, y, lv_color_hex(th->warn), 1, LV_OPA_80);
    }
    if (m->spec.danger_above > 0) {
        int y = a.y2 - (int)((m->spec.danger_above / maxv) * h);
        draw_seg(layer, a.x1, y, a.x2, y, lv_color_hex(th->err), 1, LV_OPA_80);
    }
    if (mini_vintage()) {
        /* 液面线：当前值处一条亮黄铜横线。填充用深黄铜、液面线用亮黄铜，
         * 同色会糊成一片，看不出刻度感。 */
        double ratio = m->vbar_ratio;
        if (ratio < 0) ratio = 0;
        if (ratio > 1) ratio = 1;
        int ly = a.y2 - (int)(ratio * h);
        draw_seg(layer, a.x1, ly, a.x2, ly, lv_color_hex(th->accent_2), 2,
                 LV_OPA_COVER);
    }
    int ticks = mini_vintage() ? 10 : 4;
    for (int i = 0; i <= ticks; i++) {
        int y = a.y1 + h * i / ticks;
        bool major = mini_vintage() ? (i % 5 == 0) : (i % 2 == 0);
        int len = major ? 6 : 3;
        /* 刻度画在轨道内侧左缘：overlay 尺寸等于轨道，画到外面会被裁剪 */
        draw_seg(layer, a.x1 + 1, y, a.x1 + 1 + len, y,
                 major && mini_vintage() ? lv_color_hex(th->accent_2)
                                         : lv_color_hex(th->text_mut),
                 1, major && mini_vintage() ? LV_OPA_70 : LV_OPA_50);
    }
    (void)w;
}

/* multi_bars：网格 + 柱体 */
static void multibar_cb(lv_event_t *e)
{
    ui_mini_t *m = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!m || !layer || m->n_rows <= 0) return;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const ui_theme_t *th = ui_theme();
    int x = a.x1, y = a.y1;
    int w = a.x2 - a.x1 + 1;
    int h = a.y2 - a.y1 + 1;
    int n = m->n_rows;

    double vals[UI_MINI_MAX_ROWS];
    double maxv = 1.0;
    int argmax = 0;
    for (int i = 0; i < n; i++) {
        const dash_metric_t *mt = m->rows[i].metric;
        vals[i] = (mt && mt->available && mt->type == DASH_METRIC_NUMBER &&
                   mt->number > 0)
                      ? mt->number
                      : 0.0;
        if (vals[i] > maxv) maxv = vals[i];
        if (vals[i] >= vals[argmax]) argmax = i;
    }
    maxv *= 1.15;

    /* 网格：3 条横线 + 基线 */
    bool vintage = mini_vintage();
    for (int i = 1; i <= 3; i++) {
        int gy = y + h * i / 4;
        draw_seg(layer, x, gy, x + w, gy, lv_color_hex(th->border), 1,
                 vintage ? LV_OPA_30 : LV_OPA_20);
    }
    draw_seg(layer, x, y + h, x + w, y + h, lv_color_hex(th->border), 1,
             vintage ? LV_OPA_70 : LV_OPA_50);
    if (vintage) {
        /* 基线游标尺：10 格短刻度，柱图读起来像老式记录仪 */
        for (int i = 0; i <= 10; i++) {
            int tx = x + w * i / 10;
            bool major = (i % 5 == 0);
            draw_seg(layer, tx, y + h, tx, y + h - (major ? 4 : 2),
                     major ? lv_color_hex(th->accent_2)
                           : lv_color_hex(th->border),
                     1, major ? LV_OPA_60 : LV_OPA_40);
        }
    }

    int col_w = w / n;
    int radius = th->skin == UI_SKIN_VITAL ? 3 : 2;
    for (int i = 0; i < n; i++) {
        int bh = (int)(h * vals[i] / maxv);
        if (bh < 1 && vals[i] > 0) bh = 1;
        int bw = col_w * 62 / 100;
        if (bw < 3) bw = 3;
        int bx = x + col_w * i + (col_w - bw) / 2;
        lv_color_t c = lv_color_hex(i == argmax && n > 1
                                        ? (th->skin == UI_SKIN_VINTAGE
                                               ? th->accent
                                               : th->accent_2)
                                        : mini_value_color(0, &m->spec));
        draw_rect(layer, bx, y + h - bh, bw, bh, c, radius, LV_OPA_COVER);
    }
}

/* spark_row：每列迷你折线 */
static void spark_cb(lv_event_t *e)
{
    ui_mini_t *m = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!m || !layer || m->n_rows <= 0) return;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const ui_theme_t *th = ui_theme();
    int x = a.x1, y = a.y1;
    int w = a.x2 - a.x1 + 1;
    int h = a.y2 - a.y1 + 1;
    int n = m->n_rows;
    int col_w = w / n;
    lv_color_t c = lv_color_hex(mini_value_color(0, &m->spec));

    if (mini_vintage()) {
        /* 凹陷表盘：顶/底细线 + 3 条极淡横格 + 列间分隔 */
        lv_color_t line = lv_color_hex(th->border);
        draw_seg(layer, x, y, x + w, y, line, 1, LV_OPA_40);
        draw_seg(layer, x, y + h, x + w, y + h, line, 1, LV_OPA_50);
        for (int i = 1; i <= 3; i++) {
            int gy = y + h * i / 4;
            draw_seg(layer, x, gy, x + w, gy, line, 1, LV_OPA_10);
        }
        for (int i = 1; i < n; i++) {
            int gx = x + col_w * i;
            draw_seg(layer, gx, y + 1, gx, y + h - 1, line, 1, LV_OPA_20);
        }
    }

    for (int i = 0; i < n; i++) {
        int cnt = m->rowhist_n[i];
        if (cnt < 2) continue;
        int cx0 = x + col_w * i + 2;
        int cw = col_w - 4;
        float vmin = m->rowhist[i][0], vmax = m->rowhist[i][0];
        for (int k = 0; k < cnt; k++) {
            if (m->rowhist[i][k] < vmin) vmin = m->rowhist[i][k];
            if (m->rowhist[i][k] > vmax) vmax = m->rowhist[i][k];
        }
        float span = vmax - vmin;
        if (span < 0.001f) span = 1.0f;
        lv_point_precise_t pts[UI_MINI_ROW_HIST];
        int pc = 0;
        for (int k = 0; k < cnt; k++) {
            int idx = (m->rowhist_next[i] - cnt + k + UI_MINI_ROW_HIST) %
                      UI_MINI_ROW_HIST;
            float v = m->rowhist[i][idx];
            pts[pc].x = cx0 + (int)((float)cw * k / (cnt - 1));
            pts[pc].y = y + h - 2 -
                        (int)((float)(h - 4) * (v - vmin) / span);
            pc++;
        }
#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 4)
        /* LVGL >= 9.4：折线一次绘制 */
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = c;
        d.width = 1;
        d.opa = LV_OPA_80;
        d.points = pts;
        d.point_cnt = pc;
        lv_draw_line(layer, &d);
#else
        /* 旧版只有单段 API：逐段画 */
        for (int k = 1; k < pc; k++) {
            draw_seg(layer, (int)pts[k - 1].x, (int)pts[k - 1].y, (int)pts[k].x,
                     (int)pts[k].y, c, 1, LV_OPA_80);
        }
#endif
        /* 末端采样点 */
        draw_seg(layer, (int)pts[pc - 1].x, (int)pts[pc - 1].y,
                 (int)pts[pc - 1].x, (int)pts[pc - 1].y, c, 3, LV_OPA_COVER);
    }
    (void)th;
}

/* ---------------- LED 动画 ---------------- */

static void led_tick(ui_mini_t *m)
{
    if (!m->dots[0]) return;
    const ui_theme_t *th = ui_theme();
    uint32_t ph = ui_mini_phase();
    if (m->led_on) {
        /* 呼吸：1s 周期，光晕透明度 60..220 */
        float s = (sinf(ph * 2.0f * UI_MINI_PI / 1000.0f) + 1.0f) * 0.5f;
        lv_opa_t opa = (lv_opa_t)(60 + 160 * s);
        lv_obj_set_style_outline_opa(m->dots[0], opa, 0);
    } else {
        /* 离线：1Hz 闪烁 */
        bool on = (ph / 250) % 2 == 0;
        lv_obj_set_style_outline_opa(m->dots[0], on ? LV_OPA_70 : LV_OPA_10, 0);
    }
    (void)th;
}

static void matrix_tick(ui_mini_t *m)
{
    uint32_t ph = ui_mini_phase();
    bool on = (ph / 250) % 2 == 0;
    const ui_theme_t *th = ui_theme();
    for (int i = 0; i < m->n_rows; i++) {
        if (!m->dots[i]) continue;
        const dash_metric_t *mt = m->rows[i].metric;
        bool bad = mt && mt->available && mt->type == DASH_METRIC_BOOL &&
                   !mt->boolean;
        if (bad) {
            lv_obj_set_style_bg_opa(m->dots[i], on ? LV_OPA_COVER : LV_OPA_40,
                                    0);
        } else if (lv_obj_get_style_bg_opa(m->dots[i], 0) != LV_OPA_COVER) {
            lv_obj_set_style_bg_opa(m->dots[i], LV_OPA_COVER, 0);
        }
    }
    (void)th;
}

/* ---------------- 构建：gauge_ring ---------------- */

static const lv_font_t *gauge_value_font(int diameter, int len)
{
    if (len > 5) len = 5;
    if (diameter >= 92) return len > 3 ? &lv_font_montserrat_20
                                       : &lv_font_montserrat_28;
    if (diameter >= 58) return len > 3 ? &lv_font_montserrat_14
                                       : &lv_font_montserrat_20;
    if (diameter >= 36) return len > 3 ? &lv_font_montserrat_12
                                       : &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}

static void build_gauge(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    bool compact = (bw < 54 || bh < 54);
    int diameter = LV_MIN(bw, bh) - (compact ? 0 : 4);
    if (diameter < 16) diameter = 16;
    if (diameter > 132) diameter = 132;
    int cx = bw / 2;
    int cy = compact ? bh / 2 : (bh - 10) / 2;

    lv_obj_t *arc = lv_arc_create(m->parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, diameter, diameter);
    lv_obj_set_pos(arc, cx - diameter / 2, cy - diameter / 2);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_value(arc, 62);
    int width = diameter >= 90 ? 8 : diameter >= 56 ? 6 : 4;
    if (th->skin == UI_SKIN_VITAL) width += 2;
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(th->border), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(th->accent),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, th->skin != UI_SKIN_VINTAGE,
                                 LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    m->arc = arc;

    m->value = mlabel(m->parent, "--",
                      gauge_value_font(diameter, 3), th->text);
    lv_obj_align(m->value, LV_ALIGN_CENTER, 0, diameter >= 56 ? -5 : -2);

    if (!compact) {
        m->unit = mlabel(m->parent, m->spec.unit, &lv_font_montserrat_10,
                         th->text_mut);
        lv_obj_align(m->unit, LV_ALIGN_CENTER, 0, diameter >= 90 ? 14 : 11);
    }

    if (m->spec.show_ticks && !compact && diameter >= 40) {
        m->overlay = lv_obj_create(m->parent);
        lv_obj_remove_style_all(m->overlay);
        lv_obj_set_pos(m->overlay, 0, 0);
        lv_obj_set_size(m->overlay, bw, bh);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(m->overlay, gauge_tick_cb, LV_EVENT_DRAW_MAIN_END,
                            m);
        m->g_d = diameter;
        m->g_cx = cx;
        m->g_cy = cy;
    }
}

static void apply_gauge(ui_mini_t *m)
{
    const dash_metric_t *mt = m->binds[0].metric;
    const ui_theme_t *th = ui_theme();
    char buf[32];
    bool ok = mt && mt->available && mt->type == DASH_METRIC_NUMBER;
    double v = ok ? mt->number : 0.0;
    if (!ok) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        int prec = m->binds[0].precision;
        if (prec < 0) prec = 0;
        if (prec > 2) prec = 2;
        snprintf(buf, sizeof(buf), "%.*f", prec, v);
    }
    label_set_if_changed(m->value, buf);
    int d = m->g_d > 0 ? m->g_d : 40;
    const lv_font_t *vf = gauge_value_font(d, (int)strlen(buf));
    if (lv_obj_get_style_text_font(m->value, 0) != vf) {
        lv_obj_set_style_text_font(m->value, vf, 0);
    }
    uint32_t c = ok ? mini_value_color(v, &m->spec) : th->text_mut;
    lv_obj_set_style_text_color(m->value, lv_color_hex(c), 0);
    if (m->unit && m->spec.unit[0]) {
        label_set_if_changed(m->unit, m->spec.unit);
    } else if (m->unit && m->binds[0].unit[0]) {
        label_set_if_changed(m->unit, m->binds[0].unit);
    }
    if (m->arc) {
        /* 复古皮肤：表弧用琥珀黄铜，指针用亮黄铜，二者区分开；
         * 越线仍沿用 warn/err。 */
        uint32_t arc_c = c;
        if (mini_vintage() && (c == th->accent_2 || c == th->accent)) {
            arc_c = th->accent;
        }
        lv_obj_set_style_arc_color(m->arc, lv_color_hex(arc_c),
                                   LV_PART_INDICATOR);
        double ratio = mini_ratio(m, mt, m->binds[0].unit);
        if (ratio < 0.02) ratio = 0.02;
        lv_arc_set_value(m->arc, (int32_t)(ratio * 100.0));
        m->g_ratio = ratio;
        if (m->overlay) lv_obj_invalidate(m->overlay);
    }
}

/* ---------------- 构建：led_dot ---------------- */

static void build_led(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    int side = LV_MIN(bw, bh);
    int size = side >= 88 ? 40 : side >= 64 ? 30 : side >= 44 ? 22 : 14;
    bool has_sub = (m->n_binds > 1 && side >= 64);
    bool tiny = (bh < 34);

    lv_obj_t *dot = lv_obj_create(m->parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, th->skin == UI_SKIN_VINTAGE ? 2
                                                            : LV_RADIUS_CIRCLE,
                            0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    if (th->skin == UI_SKIN_VINTAGE) {
        /* 黄铜灯座：方形灯 + 一圈细描边 */
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(th->accent_2), 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_50, 0);
    }
    lv_obj_set_style_outline_width(dot, 2, 0);
    lv_obj_set_style_outline_pad(dot, 3, 0);
    lv_obj_set_style_outline_color(dot, lv_color_hex(th->ok), 0);
    lv_obj_set_style_outline_opa(dot, LV_OPA_60, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    if (has_sub) {
        lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 2);
    } else if (tiny) {
        lv_obj_center(dot);
    } else {
        lv_obj_align(dot, LV_ALIGN_CENTER, 0, -(size / 2 + 5));
    }
    m->dots[0] = dot;

    if (!tiny) {
        const lv_font_t *tf = size >= 22 ? f_medium() : f_small();
        m->value = mlabel(m->parent, m->spec.on_text, tf, th->text);
        if (has_sub) {
            lv_obj_align(m->value, LV_ALIGN_TOP_MID, 0, size + 10);
        } else {
            lv_obj_align(m->value, LV_ALIGN_CENTER, 0, size / 2 + 7);
        }
    }

    if (has_sub) {
        m->sub = mlabel(m->parent, "--", f_small(), th->text_mut);
        lv_obj_align(m->sub, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_set_width(m->sub, bw);
        lv_label_set_long_mode(m->sub, LV_LABEL_LONG_MODE_DOTS);
    }
    m->tick = led_tick;
}

static void apply_led(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    const dash_metric_t *mt = m->binds[0].metric;
    bool on = mt && mt->available && mt->type == DASH_METRIC_BOOL &&
              mt->boolean;
    m->led_on = on;
    uint32_t c = on ? th->ok : th->err;
    if (!mt || !mt->available) c = th->text_mut;
    lv_obj_set_style_bg_color(m->dots[0], lv_color_hex(c), 0);
    lv_obj_set_style_outline_color(m->dots[0], lv_color_hex(c), 0);
    if (m->value) {
        label_set_if_changed(m->value, on ? m->spec.on_text : m->spec.off_text);
        lv_obj_set_style_text_color(m->value, lv_color_hex(c), 0);
    }
    if (m->sub) {
        const dash_metric_t *smt = m->binds[1].metric;
        char buf[48];
        fmt_metric(buf, sizeof(buf), smt, 0);
        label_set_if_changed(m->sub, buf);
    }
}

/* ---------------- 构建：led_matrix ---------------- */

static void build_matrix(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    int n = m->n_rows > 0 ? m->n_rows : 1;
    float aspect = (float)bw / (float)(bh > 0 ? bh : 1);
    /* 取最接近正方形的列数：向上取整会让 4 个灯在近方卡里排成 3+1，
     * 第三个灯孤零零落在第二行，网格明显失衡。 */
    int cols = (int)lroundf(sqrtf((float)n * aspect));
    if (cols < 1) cols = 1;
    if (cols > n) cols = n;
    int rrows = (n + cols - 1) / cols;
    int cw = bw / cols;
    int ch = bh / rrows;
    int dot = LV_MIN(cw, ch) * 42 / 100;
    if (dot > 14) dot = 14;
    if (dot < 5) dot = 5;
    bool labels = m->spec.show_labels && cw >= 36 && ch >= 15;
    /* 单元格偏高时用"点上文下"，偏宽时用"点左文右"，避免标签换行 */
    bool stack = labels && ch * 10 >= cw * 8;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;
        lv_obj_t *d = lv_obj_create(m->parent);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_style_radius(
            d, th->skin == UI_SKIN_VINTAGE ? 1 : LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(th->text_mut), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        if (th->skin == UI_SKIN_VINTAGE && dot >= 8) {
            lv_obj_set_style_border_width(d, 1, 0);
            lv_obj_set_style_border_color(d, lv_color_hex(th->accent_2), 0);
            lv_obj_set_style_border_opa(d, LV_OPA_40, 0);
        }
        if (stack) {
            lv_obj_set_pos(d, col * cw + (cw - dot) / 2, row * ch + 2);
        } else {
            lv_obj_set_pos(d, col * cw + 2, row * ch + (ch - dot) / 2);
        }
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        m->dots[i] = d;

        if (labels) {
            m->labels[i] = mlabel(m->parent, m->rows[i].label, f_small(),
                                  th->text_dim);
            if (stack) {
                lv_obj_set_pos(m->labels[i], col * cw + 3, row * ch + dot + 5);
                lv_obj_set_width(m->labels[i], cw - 6);
                lv_obj_set_style_text_align(m->labels[i], LV_TEXT_ALIGN_CENTER,
                                            0);
            } else {
                lv_obj_set_pos(m->labels[i], col * cw + dot + 5,
                               row * ch + (ch - f_small()->line_height) / 2);
                lv_obj_set_width(m->labels[i], cw - dot - 7);
            }
            lv_label_set_long_mode(m->labels[i], LV_LABEL_LONG_MODE_DOTS);
        }
    }
    m->tick = matrix_tick;
}

static void apply_matrix(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    bool any_off = false;
    for (int i = 0; i < m->n_rows; i++) {
        const dash_metric_t *mt = m->rows[i].metric;
        uint32_t c = th->text_mut;
        if (mt && mt->available && mt->type == DASH_METRIC_BOOL) {
            c = mt->boolean ? th->ok : th->err;
            if (!mt->boolean) any_off = true;
        }
        if (m->dots[i]) {
            lv_obj_set_style_bg_color(m->dots[i], lv_color_hex(c), 0);
            if (!any_off || !mt || mt->boolean || !mt->available) {
                lv_obj_set_style_bg_opa(m->dots[i], LV_OPA_COVER, 0);
            }
        }
    }
}

/* ---------------- 构建：bar_strip ---------------- */

static void make_hbar(lv_obj_t *parent, int x, int y, int w, int h,
                      lv_obj_t **track, lv_obj_t **fill)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, h / 2, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(mini_track_color()), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *f = lv_obj_create(t);
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, 1, h);
    lv_obj_set_style_radius(f, h / 2, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(th->accent), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    *track = t;
    *fill = f;
}

static void attach_bar_ticks(ui_mini_t *m, int x, int y, int w, int h)
{
    m->overlay = lv_obj_create(m->parent);
    lv_obj_remove_style_all(m->overlay);
    lv_obj_set_pos(m->overlay, x, y);
    lv_obj_set_size(m->overlay, w, h);
    lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m->overlay, bar_tick_cb, LV_EVENT_DRAW_MAIN_END, m);
    lv_obj_move_foreground(m->overlay);
}

static void build_bar(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    const char *unit = m->spec.unit[0] ? m->spec.unit : m->binds[0].unit;
    m->bar_max = mini_max_of(m, m->binds[0].metric, unit);

    if (m->gh <= 2) {
        /* inline：标签 | 条 | 数值 */
        int label_w = bw * 30 / 100;
        if (label_w > 72) label_w = 72;
        if (label_w < 26) label_w = LV_MIN(26, bw / 3);
        int value_w = 54;
        m->labels[0] = mlabel(m->parent, m->spec.title, f_small(), th->text_dim);
        lv_obj_set_pos(m->labels[0], 0, (bh - f_small()->line_height) / 2);
        lv_obj_set_width(m->labels[0], label_w);
        lv_label_set_long_mode(m->labels[0], LV_LABEL_LONG_MODE_DOTS);
        int bar_x = label_w + 6;
        int bar_w = bw - bar_x - value_w - 8;
        if (bar_w < 16) bar_w = 16;
        make_hbar(m->parent, bar_x, (bh - 8) / 2, bar_w, 8, &m->track,
                  &m->fill);
        m->track_w = bar_w;
        m->track_h = 8;
        m->value = mlabel(m->parent, "--", &lv_font_montserrat_14, th->text);
        lv_obj_set_pos(m->value, bw - value_w, (bh - 16) / 2);
        lv_obj_set_width(m->value, value_w);
        lv_obj_set_style_text_align(m->value, LV_TEXT_ALIGN_RIGHT, 0);
        if (mini_vintage() || m->spec.warn_above > 0 ||
            m->spec.danger_above > 0) {
            attach_bar_ticks(m, bar_x, (bh - 8) / 2, bar_w, 8);
        }
    } else if (m->gh == 3) {
        /* 堆叠：标题+数值 / 条 */
        const lv_font_t *vf = bw >= 150 ? &lv_font_montserrat_20
                              : bw >= 90 ? &lv_font_montserrat_16
                                         : &lv_font_montserrat_14;
        m->labels[0] = mlabel(m->parent, mini_inline_title(m), f_small(),
                              th->text_dim);
        lv_obj_set_pos(m->labels[0], 0, 1);
        lv_obj_set_width(m->labels[0], bw * 55 / 100);
        lv_label_set_long_mode(m->labels[0], LV_LABEL_LONG_MODE_DOTS);
        m->value = mlabel(m->parent, "--", vf, th->text);
        lv_obj_set_pos(m->value, bw * 45 / 100, 0);
        lv_obj_set_width(m->value, bw * 55 / 100);
        lv_obj_set_style_text_align(m->value, LV_TEXT_ALIGN_RIGHT, 0);
        make_hbar(m->parent, 0, bh - 12, bw, 10, &m->track, &m->fill);
        m->track_w = bw;
        m->track_h = 10;
        if (mini_vintage() || m->spec.warn_above > 0 ||
            m->spec.danger_above > 0) {
            attach_bar_ticks(m, 0, bh - 12, bw, 10);
        }
    } else {
        /* 12x4 主数值卡：左 hero 数值，右粗条 */
        int left_w = bw * 42 / 100;
        const lv_font_t *vf = bw >= 200 ? &lv_font_montserrat_40
                              : bw >= 140 ? &lv_font_montserrat_28
                                         : &lv_font_montserrat_20;
        m->labels[0] = mlabel(m->parent, mini_inline_title(m), f_small(),
                              th->text_dim);
        lv_obj_set_pos(m->labels[0], 0, 2);
        lv_obj_set_width(m->labels[0], left_w);
        m->value = mlabel(m->parent, "--", vf, th->text);
        lv_obj_set_pos(m->value, 0, 16);
        lv_obj_set_width(m->value, left_w);
        m->unit = mlabel(m->parent, unit, &lv_font_montserrat_12, th->text_mut);
        lv_obj_align_to(m->unit, m->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);

        int bar_w = bw - left_w - 18;
        if (bar_w > 30) bar_w = 30;
        int bar_x = bw - bar_w;
        lv_obj_t *t = lv_obj_create(m->parent);
        lv_obj_remove_style_all(t);
        lv_obj_set_pos(t, bar_x, 2);
        lv_obj_set_size(t, bar_w, bh - 4);
        lv_obj_set_style_radius(t, 4, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(mini_track_color()), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *f = lv_obj_create(t);
        lv_obj_remove_style_all(f);
        lv_obj_set_width(f, bar_w);
        lv_obj_set_height(f, 2);
        lv_obj_set_style_radius(f, 4, 0);
        lv_obj_set_style_bg_color(f, lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
        m->track = t;
        m->fill = f;
        m->track_w = bar_w;
        m->track_h = bh - 4;
        m->overlay = lv_obj_create(m->parent);
        lv_obj_remove_style_all(m->overlay);
        lv_obj_set_pos(m->overlay, bar_x, 2);
        lv_obj_set_size(m->overlay, bar_w, bh - 4);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(m->overlay, vbar_tick_cb, LV_EVENT_DRAW_MAIN_END,
                            m);
        lv_obj_move_foreground(m->overlay);
    }
}

static void apply_bar(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    const dash_metric_t *mt = m->binds[0].metric;
    bool ok = mt && mt->available && mt->type == DASH_METRIC_NUMBER;
    double v = ok ? mt->number : 0.0;
    char buf[40];
    if (!ok) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        int prec = m->binds[0].precision;
        if (prec < 0) prec = 0;
        if (prec > 2) prec = 2;
        snprintf(buf, sizeof(buf), "%.*f", prec, v);
        const char *unit = m->spec.unit[0] ? m->spec.unit : m->binds[0].unit;
        if (unit && unit[0]) {
            size_t len = strlen(buf);
            snprintf(buf + len, sizeof(buf) - len, " %s", unit);
        }
    }
    label_set_if_changed(m->value, buf);
    uint32_t c = ok ? mini_value_color(v, &m->spec) : th->text_mut;
    lv_obj_set_style_text_color(m->value, lv_color_hex(c), 0);
    double ratio = mini_ratio(m, mt, m->binds[0].unit);
    if (ratio < 0.02) ratio = 0.02;
    if (m->fill && m->track) {
        /* 复古皮肤：正常态填充深黄铜（accent），与圆表弧、竖条液面同一套
         * 金属层次；数值文字仍是亮黄铜，二者才分得开。 */
        uint32_t fill_c = c;
        if (mini_vintage() && (c == th->accent_2 || c == th->accent)) {
            fill_c = th->accent;
        }
        lv_obj_set_style_bg_color(m->fill, lv_color_hex(fill_c), 0);
        if (m->gh >= 4) {
            lv_obj_set_height(m->fill, (int)(m->track_h * ratio));
            lv_obj_align(m->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        } else {
            lv_obj_set_width(m->fill, (int)(m->track_w * ratio));
        }
    }
}

/* ---------------- 构建：vbar_gauge ---------------- */

static void build_vbar(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    const char *unit = m->spec.unit[0] ? m->spec.unit : m->binds[0].unit;
    m->bar_max = mini_max_of(m, m->binds[0].metric, unit);

    int track_w = bw * 40 / 100;
    if (track_w > 22) track_w = 22;
    if (track_w < 8) track_w = 8;
    int value_h = bh >= 120 ? 34 : bh >= 80 ? 26 : 20;
    int track_h = bh - value_h;
    if (track_h < 16) {
        track_h = 16;
        value_h = bh - 16;
        if (value_h < 10) value_h = 10;
    }
    int tx = bw >= 48 ? bw * 36 / 100 - track_w / 2 : (bw - track_w) / 2;
    if (tx < 0) tx = 0;

    lv_obj_t *t = lv_obj_create(m->parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_pos(t, tx, 0);
    lv_obj_set_size(t, track_w, track_h);
    lv_obj_set_style_radius(t, th->skin == UI_SKIN_VITAL ? 5 : 3, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(mini_track_color()), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    m->track = t;
    m->track_w = track_w;
    m->track_h = track_h;

    lv_obj_t *f = lv_obj_create(t);
    lv_obj_remove_style_all(f);
    lv_obj_set_width(f, track_w);
    lv_obj_set_height(f, 2);
    lv_obj_set_style_radius(f, th->skin == UI_SKIN_VITAL ? 5 : 3, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(th->accent), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    m->fill = f;

    const lv_font_t *vf = bh >= 120 ? &lv_font_montserrat_20
                       : bh >= 80 ? &lv_font_montserrat_16
                                  : &lv_font_montserrat_12;
    m->value = mlabel(m->parent, "--", vf, th->text);
    lv_obj_set_pos(m->value, 0, track_h + 2);
    lv_obj_set_width(m->value, bw);
    lv_obj_set_style_text_align(m->value, LV_TEXT_ALIGN_CENTER, 0);
    if (bh >= 80 && unit && unit[0]) {
        m->unit = mlabel(m->parent, unit, &lv_font_montserrat_10, th->text_mut);
        lv_obj_set_pos(m->unit, 0, track_h + 2 + vf->line_height + 1);
        lv_obj_set_width(m->unit, bw);
        lv_obj_set_style_text_align(m->unit, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (track_h >= 24 &&
        (mini_vintage() || m->spec.warn_above > 0 ||
         m->spec.danger_above > 0)) {
        m->overlay = lv_obj_create(m->parent);
        lv_obj_remove_style_all(m->overlay);
        lv_obj_set_pos(m->overlay, tx, 0);
        lv_obj_set_size(m->overlay, track_w, track_h);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(m->overlay, vbar_tick_cb, LV_EVENT_DRAW_MAIN_END,
                            m);
    }
}

static void apply_vbar(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    const dash_metric_t *mt = m->binds[0].metric;
    bool ok = mt && mt->available && mt->type == DASH_METRIC_NUMBER;
    double v = ok ? mt->number : 0.0;
    char buf[32];
    if (!ok) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        int prec = m->binds[0].precision;
        if (prec < 0) prec = 0;
        if (prec > 2) prec = 2;
        snprintf(buf, sizeof(buf), "%.*f", prec, v);
    }
    label_set_if_changed(m->value, buf);
    uint32_t c = ok ? mini_value_color(v, &m->spec) : th->text_mut;
    lv_obj_set_style_text_color(m->value, lv_color_hex(c), 0);
    double ratio = mini_ratio(m, mt, m->binds[0].unit);
    if (ratio < 0.03) ratio = 0.03;
    if (m->fill && m->track) {
        /* 复古皮肤：正常态用深黄铜填充，把亮黄铜留给液面线；
         * 越线仍用 warn/err，保证告警语义优先。 */
        uint32_t fill_c = c;
        bool normal = (c == th->accent_2 || c == th->accent);
        if (mini_vintage() && normal) fill_c = th->accent;
        lv_obj_set_height(m->fill, (int)(m->track_h * ratio));
        lv_obj_set_style_bg_color(m->fill, lv_color_hex(fill_c), 0);
        lv_obj_align(m->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    m->vbar_ratio = ratio;
    if (m->overlay) lv_obj_invalidate(m->overlay);
}

/* ---------------- 构建：multi_bars ---------------- */

static void build_multibar(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    int n = m->n_rows > 0 ? m->n_rows : 1;
    int top = (m->spec.show_values && bw >= 48 && n <= 6) ? 11 : 0;
    int bottom = m->spec.show_labels ? 12 : 0;
    m->mb_top = top;
    m->mb_bottom = bottom;

    m->overlay = lv_obj_create(m->parent);
    lv_obj_remove_style_all(m->overlay);
    lv_obj_set_pos(m->overlay, 0, top);
    lv_obj_set_size(m->overlay, bw, bh - top - bottom);
    lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m->overlay, multibar_cb, LV_EVENT_DRAW_MAIN_END, m);

    int col_w = bw / n;
    for (int i = 0; i < n; i++) {
        if (top) {
            m->values[i] = mlabel(m->parent, "--", &lv_font_montserrat_10,
                                  th->text);
            lv_obj_set_pos(m->values[i], col_w * i, 0);
            lv_obj_set_width(m->values[i], col_w);
            lv_obj_set_style_text_align(m->values[i], LV_TEXT_ALIGN_CENTER, 0);
        }
        if (bottom) {
            bool ascii = text_is_ascii(m->rows[i].label);
            m->labels[i] = mlabel(m->parent, m->rows[i].label,
                                  ascii ? &lv_font_montserrat_10 : f_small(),
                                  th->text_dim);
            lv_obj_set_pos(m->labels[i], col_w * i, bh - bottom + 1);
            lv_obj_set_width(m->labels[i], col_w);
            lv_obj_set_style_text_align(m->labels[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(m->labels[i], LV_LABEL_LONG_MODE_DOTS);
        }
    }
}

static void apply_multibar(ui_mini_t *m)
{
    for (int i = 0; i < m->n_rows; i++) {
        if (!m->values[i]) continue;
        const dash_metric_t *mt = m->rows[i].metric;
        char buf[24];
        if (!mt || !mt->available || mt->type != DASH_METRIC_NUMBER) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            int prec = m->rows[i].precision;
            if (prec < 0) prec = 0;
            if (prec > 2) prec = 2;
            double v = mt->number;
            if (v >= 1000) {
                snprintf(buf, sizeof(buf), "%.1fk", v / 1000.0);
            } else {
                snprintf(buf, sizeof(buf), "%.*f", prec, v);
            }
        }
        label_set_if_changed(m->values[i], buf);
    }
    if (m->overlay) lv_obj_invalidate(m->overlay);
}

/* ---------------- 构建：dual_line ---------------- */

/* dual_line：主线下方面积填充。折线图无原生面积填充，这里按 2px 列
 * 对系列点线性插值，画低透明度竖列近似，视觉上得到平滑面积。 */
static void dual_area_cb(lv_event_t *e)
{
    ui_mini_t *m = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!m || !layer || !m->chart || !m->ser1) return;
    lv_area_t ca;
    lv_obj_get_coords(m->chart, &ca);
    int bottom = ca.y2;
    int cnt = lv_chart_get_point_count(m->chart);
    if (cnt < 2 || cnt > UI_MINI_HIST) cnt = UI_MINI_HIST;
    lv_point_t pts[UI_MINI_HIST];
    int npts = 0;
    for (int i = 0; i < cnt; i++) {
        lv_point_t p;
        lv_chart_get_point_pos_by_id(m->chart, m->ser1, (uint32_t)i, &p);
        p.x += ca.x1; /* API 返回相对坐标 */
        p.y += ca.y1;
        if (p.y < ca.y1 - 1 || p.y > bottom + 1) continue; /* 异常值跳过 */
        pts[npts++] = p;
    }
    if (npts < 2) return;
    const ui_theme_t *th = ui_theme();
    lv_color_t c = lv_color_hex(th->accent);
    int last_x = pts[npts - 1].x;
    for (int x = pts[0].x; x <= last_x; x += 2) {
        int seg = 0;
        while (seg < npts - 2 && pts[seg + 1].x < x) seg++;
        int x0 = pts[seg].x, x1 = pts[seg + 1].x;
        int y = (x1 == x0) ? pts[seg].y
                           : pts[seg].y +
                                 (pts[seg + 1].y - pts[seg].y) * (x - x0) /
                                     (x1 - x0);
        if (y < ca.y1) y = ca.y1;
        if (y > bottom) y = bottom;
        int xr = x + 1;
        if (xr > last_x) xr = last_x;
        draw_rect(layer, x, y, xr - x + 1, bottom - y + 1, c, 0, LV_OPA_20);
    }
}

static void build_dual(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    int legend_h = 16;
    int chart_h = bh - legend_h;
    if (chart_h < 20) {
        legend_h = 0;
        chart_h = bh;
    }

    if (legend_h) {
        int half = bw / 2;
        int name_w = half * 30 / 100;
        int value_w = half - 10 - name_w;
        for (int s = 0; s < 2; s++) {
            lv_obj_t *dot = lv_obj_create(m->parent);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(
                dot, lv_color_hex(s == 0 ? th->accent : th->accent_2), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_pos(dot, s * half, 2);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            m->dots[s] = dot;

            const char *name = s == 0 ? m->binds[0].label
                                      : m->binds[1].label;
            m->labels[s] = mlabel(m->parent, name, f_small(), th->text_dim);
            lv_obj_set_pos(m->labels[s], s * half + 10, 0);
            lv_obj_set_width(m->labels[s], name_w);
            lv_label_set_long_mode(m->labels[s], LV_LABEL_LONG_MODE_DOTS);

            m->values[s] = mlabel(m->parent, "--", &lv_font_montserrat_12,
                                  s == 0 ? th->accent : th->accent_2);
            lv_obj_set_pos(m->values[s], s * half + 10 + name_w, 1);
            lv_obj_set_width(m->values[s], value_w);
            /* 数值带单位会超宽：限成单行省略，别折行压到图上 */
            lv_label_set_long_mode(m->values[s], LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_align(m->values[s], LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    m->chart = ui_chart_create(m->parent, chart_h, th->accent, th->accent_2,
                               &m->ser1, &m->ser2);
    lv_obj_set_pos(m->chart, 0, legend_h);
    if (mini_vintage()) {
        /* 机械记录纸：横向 3 格 + 纵向 4 格淡格线 */
        lv_chart_set_div_line_count(m->chart, 3, 4);
        lv_obj_set_style_line_width(m->chart, 1, LV_PART_MAIN);
        lv_obj_set_style_line_color(m->chart, lv_color_hex(th->border),
                                    LV_PART_MAIN);
        lv_obj_set_style_line_opa(m->chart, LV_OPA_30, LV_PART_MAIN);
    } else {
        lv_chart_set_div_line_count(m->chart, 3, 0);
    }
    lv_chart_set_range(m->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    if (m->ser2) {
        lv_chart_set_range(m->chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    }
    lv_obj_move_background(m->chart);
    if (m->spec.show_area) {
        /* 面积填充覆盖层：与 chart 同几何，绘制事件里补画 */
        m->overlay = lv_obj_create(m->parent);
        lv_obj_remove_style_all(m->overlay);
        lv_obj_set_pos(m->overlay, 0, legend_h);
        lv_obj_set_size(m->overlay, bw, chart_h);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(m->overlay, dual_area_cb, LV_EVENT_DRAW_MAIN_END,
                            m);
        lv_obj_move_foreground(m->overlay);
    }
}

static void push_hist(float *hist, int *n, int *next, float v)
{
    hist[*next] = v;
    *next = (*next + 1) % UI_MINI_HIST;
    if (*n < UI_MINI_HIST) (*n)++;
}

static void apply_dual(ui_mini_t *m)
{
    const dash_metric_t *a = m->binds[0].metric;
    const dash_metric_t *b = m->binds[1].metric;
    bool oka = a && a->available && a->type == DASH_METRIC_NUMBER;
    bool okb = b && b->available && b->type == DASH_METRIC_NUMBER;
    float va = oka ? (float)a->number : 0.0f;
    float vb = okb ? (float)b->number : 0.0f;

    if (m->ser1 && oka) {
        push_hist(m->hist, &m->hist_n, &m->hist_next, va);
        float lo = va, hi = va;
        for (int i = 0; i < m->hist_n; i++) {
            if (m->hist[i] < lo) lo = m->hist[i];
            if (m->hist[i] > hi) hi = m->hist[i];
        }
        if (hi - lo < 1.0f) hi = lo + 1.0f;
        auto_range(va, &lo, &hi);
        lv_chart_set_range(m->chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lo,
                           (int32_t)hi + 1);
        if (m->hist_n == 1) {
            lv_chart_set_all_values(m->chart, m->ser1, (int32_t)va);
        } else {
            lv_chart_set_next_value(m->chart, m->ser1, (int32_t)va);
        }
    }
    if (m->ser2 && okb) {
        push_hist(m->hist2, &m->hist2_n, &m->hist2_next, vb);
        float lo = vb, hi = vb;
        for (int i = 0; i < m->hist2_n; i++) {
            if (m->hist2[i] < lo) lo = m->hist2[i];
            if (m->hist2[i] > hi) hi = m->hist2[i];
        }
        if (hi - lo < 1.0f) hi = lo + 1.0f;
        auto_range(vb, &lo, &hi);
        lv_chart_set_range(m->chart, LV_CHART_AXIS_SECONDARY_Y, (int32_t)lo,
                           (int32_t)hi + 1);
        if (m->hist2_n == 1) {
            lv_chart_set_all_values(m->chart, m->ser2, (int32_t)vb);
        } else {
            lv_chart_set_next_value(m->chart, m->ser2, (int32_t)vb);
        }
    }
    for (int s = 0; s < 2; s++) {
        if (!m->values[s]) continue;
        const dash_metric_t *mt = s == 0 ? a : b;
        const char *unit = s == 0 ? m->spec.unit : m->spec.unit2;
        if (!unit[0]) unit = m->binds[s].unit;
        char buf[32];
        if (!mt || !mt->available || mt->type != DASH_METRIC_NUMBER) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            int prec = m->binds[s].precision;
            if (prec < 0) prec = 0;
            if (prec > 2) prec = 2;
            snprintf(buf, sizeof(buf), "%.*f%s%s", prec, mt->number,
                     unit[0] ? " " : "", unit);
        }
        label_set_if_changed(m->values[s], buf);
    }
    /* 面积覆盖层不随 chart 自动重绘，数值推进后手动失效 */
    if (m->overlay) lv_obj_invalidate(m->overlay);
}

/* ---------------- 构建：big_stat ---------------- */

static void build_big(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    bool roomy = (bh >= 66 && bw >= 60);

    if (roomy) {
        m->chart = ui_chart_create(m->parent, bh - 44, th->accent, 0, &m->ser1,
                                   NULL);
        lv_obj_set_pos(m->chart, 0, 0);
        lv_obj_set_width(m->chart, LV_PCT(100));
        lv_obj_set_style_line_opa(m->chart, LV_OPA_20, LV_PART_ITEMS);
        lv_chart_set_div_line_count(m->chart, 0, 0);
        lv_chart_set_range(m->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_obj_move_background(m->chart);
    }

    const lv_font_t *vf = bw >= 110 && bh >= 90 ? &lv_font_montserrat_48
                        : bw >= 80 ? &lv_font_montserrat_28
                                   : &lv_font_montserrat_20;
    m->value = mlabel(m->parent, "--", vf, th->accent);
    if (roomy) {
        lv_obj_align(m->value, LV_ALIGN_TOP_LEFT, 2, 4);
    } else {
        lv_obj_align(m->value, LV_ALIGN_CENTER, 0, -6);
    }

    const char *unit = m->spec.unit[0] ? m->spec.unit : m->binds[0].unit;
    if (unit[0]) {
        m->unit = mlabel(m->parent, unit, &lv_font_montserrat_12, th->text_mut);
        if (roomy) {
            lv_obj_align_to(m->unit, m->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 4,
                            -6);
        } else {
            lv_obj_align_to(m->unit, m->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 3,
                            -4);
        }
    }

    if (m->spec.show_trend) {
        m->trend = mlabel(m->parent, "--", f_small(), th->text_mut);
        if (roomy) {
            lv_obj_align(m->trend, LV_ALIGN_BOTTOM_LEFT, 2, -2);
        } else {
            lv_obj_align(m->trend, LV_ALIGN_BOTTOM_MID, 0, -1);
        }
        lv_obj_set_width(m->trend, bw - 4);
    }
}

static void apply_big(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    const dash_metric_t *mt = m->binds[0].metric;
    bool ok = mt && mt->available && mt->type == DASH_METRIC_NUMBER;
    double v = ok ? mt->number : 0.0;
    char buf[32];
    if (!ok) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        int prec = m->binds[0].precision;
        if (prec < 0) prec = 0;
        if (prec > 2) prec = 2;
        snprintf(buf, sizeof(buf), "%.*f", prec, v);
        /* 数值过长时缩一档字号 */
        const lv_font_t *vf = lv_obj_get_style_text_font(m->value, 0);
        if (strlen(buf) > 4 && vf == &lv_font_montserrat_48) {
            lv_obj_set_style_text_font(m->value, &lv_font_montserrat_28, 0);
        } else if (strlen(buf) > 5 && vf == &lv_font_montserrat_28) {
            lv_obj_set_style_text_font(m->value, &lv_font_montserrat_20, 0);
        }
    }
    label_set_if_changed(m->value, buf);
    uint32_t c = ok ? mini_value_color(v, &m->spec) : th->text_mut;
    lv_obj_set_style_text_color(m->value, lv_color_hex(c), 0);

    if (ok && m->chart && m->ser1) {
        push_hist(m->hist, &m->hist_n, &m->hist_next, (float)v);
        if (m->hist_n == 1) {
            lv_chart_set_all_values(m->chart, m->ser1, (int32_t)v);
        } else {
            lv_chart_set_next_value(m->chart, m->ser1, (int32_t)v);
        }
        float lo = (float)v, hi = (float)v;
        for (int i = 0; i < m->hist_n; i++) {
            if (m->hist[i] < lo) lo = m->hist[i];
            if (m->hist[i] > hi) hi = m->hist[i];
        }
        if (hi - lo < 1.0f) hi = lo + 1.0f;
        auto_range((float)v, &lo, &hi);
        lv_chart_set_range(m->chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lo,
                           (int32_t)hi + 1);
    }

    if (m->trend && m->hist_n >= 2) {
        /* 环形缓冲未满时最早样本在 0，满后最早样本即 next 槽 */
        int oldest = (m->hist_n < UI_MINI_HIST) ? 0 : m->hist_next;
        float old = m->hist[oldest];
        char t[40];
        if (ok && old > 0.05f) {
            float pct = (float)((v - old) / old * 100.0);
            uint32_t tc = pct >= 0 ? th->ok : th->err;
            /* 箭头字符不在 CJK 子集里，会渲染成方框；涨跌由颜色表达。 */
            snprintf(t, sizeof(t), "%+.1f%% / 60s", pct);
            lv_obj_set_style_text_color(m->trend, lv_color_hex(tc), 0);
        } else {
            /* 运行期新增的中文不在服务端字体子集里，会渲染成方框 */
            snprintf(t, sizeof(t), "--");
            lv_obj_set_style_text_color(m->trend, lv_color_hex(th->text_mut), 0);
        }
        label_set_if_changed(m->trend, t);
    }
}

/* ---------------- 构建：spark_row ---------------- */

static void build_sparkrow(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;
    int n = m->n_rows > 0 ? m->n_rows : 1;
    int top = 13;
    int bottom = (m->spec.show_labels && bh >= 30) ? 11 : 0;
    m->sp_label_h = bottom;
    int col_w = bw / n;

    for (int i = 0; i < n; i++) {
        m->col_w[i] = col_w - 4;
        m->values[i] = mlabel(m->parent, "--", &lv_font_montserrat_12, th->text);
        lv_obj_set_pos(m->values[i], col_w * i + 2, 0);
        lv_obj_set_width(m->values[i], col_w - 4);
        lv_obj_set_style_text_align(m->values[i], LV_TEXT_ALIGN_CENTER, 0);
        if (bottom) {
            bool ascii = text_is_ascii(m->rows[i].label);
            m->labels[i] = mlabel(m->parent, m->rows[i].label,
                                  ascii ? &lv_font_montserrat_10 : f_small(),
                                  th->text_dim);
            lv_obj_set_pos(m->labels[i], col_w * i + 2, bh - bottom + 1);
            lv_obj_set_width(m->labels[i], col_w - 4);
            lv_obj_set_style_text_align(m->labels[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(m->labels[i], LV_LABEL_LONG_MODE_DOTS);
        }
    }
    m->overlay = lv_obj_create(m->parent);
    lv_obj_remove_style_all(m->overlay);
    lv_obj_set_pos(m->overlay, 0, top);
    lv_obj_set_size(m->overlay, bw, bh - top - bottom);
    lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m->overlay, spark_cb, LV_EVENT_DRAW_MAIN_END, m);
}

static void apply_sparkrow(ui_mini_t *m)
{
    for (int i = 0; i < m->n_rows; i++) {
        const dash_metric_t *mt = m->rows[i].metric;
        float v = (mt && mt->available && mt->type == DASH_METRIC_NUMBER)
                      ? (float)mt->number
                      : 0.0f;
        m->rowhist[i][m->rowhist_next[i]] = v;
        m->rowhist_next[i] =
            (m->rowhist_next[i] + 1) % UI_MINI_ROW_HIST;
        if (m->rowhist_n[i] < UI_MINI_ROW_HIST) m->rowhist_n[i]++;
        if (m->values[i]) {
            char buf[24];
            if (!mt || !mt->available || mt->type != DASH_METRIC_NUMBER) {
                snprintf(buf, sizeof(buf), "--");
            } else {
                int prec = m->rows[i].precision;
                if (prec < 0) prec = 0;
                if (prec > 1) prec = 1;
                const char *unit = m->rows[i].unit;
                if (unit[0] && m->col_w[i] >= 40) {
                    snprintf(buf, sizeof(buf), "%.*f %s", prec, v, unit);
                } else {
                    snprintf(buf, sizeof(buf), "%.*f", prec, v);
                }
            }
            label_set_if_changed(m->values[i], buf);
        }
    }
    if (m->overlay) lv_obj_invalidate(m->overlay);
}

/* ---------------- 构建：text_badge ---------------- */

static const lv_font_t *badge_font(const char *text, int bw, int bh,
                                   int tier, bool *cjk)
{
    bool ascii = text_is_ascii(text);
    *cjk = !ascii;
    int len = (int)strlen(text);
    if (len > 24) len = 24;
    /* 最长不可断词（数字/单词）：LVGL 会在空格处折行，但数字串一旦比
     * 列宽还长就会被从中间劈开（4729 → 47/29）。选字号时先保证它一行放得下。 */
    int token = 0, cur = 0;
    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            cur = 0;
            continue;
        }
        if (++cur > token) token = cur;
    }
    if (token > 24) token = 24;
    const lv_font_t *ladder_ascii[] = {&lv_font_montserrat_28,
                                       &lv_font_montserrat_20,
                                       &lv_font_montserrat_16,
                                       &lv_font_montserrat_14,
                                       &lv_font_montserrat_12,
                                       &lv_font_montserrat_10};
    const lv_font_t *ladder_cjk[] = {f_large(), f_medium(), f_small(),
                                     f_small(), f_small(), f_small()};
    const lv_font_t **ladder = ascii ? ladder_ascii : ladder_cjk;
    int start = 2 - tier; /* S→2 M→1 L→0 */
    if (start < 0) start = 0;
    if (start > 2) start = 2;
    int lines = bh >= 40 ? bh / 18 : 1;
    for (int i = start; i < 6; i++) {
        const lv_font_t *f = ladder[i];
        int cw = ascii ? (int)(f->line_height * 0.62f) : f->line_height;
        if (token * cw > bw) continue;
        if (len * cw <= bw * lines) return f;
    }
    return ladder[5];
}

static void build_text(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    int bw = m->body_w, bh = m->body_h;

    /* 左侧装饰竖条 */
    if (bh >= 30) {
        lv_obj_t *bar = lv_obj_create(m->parent);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 0, 1);
        lv_obj_set_size(bar, 3, bh - 2);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(
            bar, lv_color_hex(th->skin == UI_SKIN_VINTAGE ? th->accent_2
                                                        : th->accent),
            0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }

    m->value = mlabel(m->parent, "--", f_medium(), th->text);
    /* 窄列（2 格）左右留白压到 4px，否则 4 位数字放不下，
     * 会被 LVGL 从中间劈成两行。 */
    int pad = bh >= 30 ? (bw < 60 ? 4 : 8) : 2;
    lv_obj_set_pos(m->value, pad, 0);
    lv_obj_set_width(m->value, bw - pad);
    m->value_w = bw - pad;
    lv_obj_set_height(m->value, bh);
    if (bh >= 40) {
        lv_label_set_long_mode(m->value, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(
            m->value, m->spec.align == 1 ? LV_TEXT_ALIGN_CENTER
                                         : LV_TEXT_ALIGN_LEFT, 0);
    } else {
        lv_label_set_long_mode(m->value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(
            m->value, m->spec.align == 1 ? LV_TEXT_ALIGN_CENTER
                                         : LV_TEXT_ALIGN_LEFT, 0);
    }
    lv_obj_set_style_text_line_space(m->value, 2, 0);
}

static void apply_text(ui_mini_t *m)
{
    const ui_theme_t *th = ui_theme();
    const dash_metric_t *mt = m->binds[0].metric;
    char buf[96];
    uint32_t c = th->text;
    if (!mt || !mt->available) {
        snprintf(buf, sizeof(buf), "--");
        c = th->text_mut;
    } else if (mt->type == DASH_METRIC_BOOL) {
        snprintf(buf, sizeof(buf), "%s", mt->boolean ? "在线" : "离线");
        c = mt->boolean ? th->ok : th->err;
    } else if (mt->type == DASH_METRIC_NUMBER) {
        int prec = m->binds[0].precision;
        if (prec < 0) prec = 0;
        if (prec > 3) prec = 3;
        const char *unit = m->spec.unit[0] ? m->spec.unit : m->binds[0].unit;
        /* 窄列只放数字：单位会把 4 位数字挤成两行，拆在数位中间更难读。
         * 单位交给卡片标题（如“运行 s”）。 */
        if (unit[0] && m->value_w >= 56) {
            snprintf(buf, sizeof(buf), "%.*f %s", prec, mt->number, unit);
        } else {
            snprintf(buf, sizeof(buf), "%.*f", prec, mt->number);
        }
        c = mini_value_color(mt->number, &m->spec);
    } else {
        snprintf(buf, sizeof(buf), "%s", mt->text[0] ? mt->text : "--");
        if (th->skin == UI_SKIN_NEON) c = th->accent;
    }
    label_set_if_changed(m->value, buf);
    lv_obj_set_style_text_color(m->value, lv_color_hex(c), 0);
    bool cjk = false;
    const lv_font_t *f =
        badge_font(buf, m->value_w, m->body_h,
                   m->spec.font_size, &cjk);
    if (lv_obj_get_style_text_font(m->value, 0) != f) {
        lv_obj_set_style_text_font(m->value, f, 0);
    }
}

/* ---------------- 类型表 ---------------- */

typedef struct {
    const char *type;
    const char *label;
    void (*build)(ui_mini_t *m);
    void (*apply)(ui_mini_t *m);
} mini_entry_t;

static const mini_entry_t s_entries[] = {
    {"gauge_ring", "环形仪表", build_gauge, apply_gauge},
    {"led_dot", "LED 状态灯", build_led, apply_led},
    {"led_matrix", "点阵状态墙", build_matrix, apply_matrix},
    {"bar_strip", "指标条形带", build_bar, apply_bar},
    {"vbar_gauge", "垂直柱", build_vbar, apply_vbar},
    {"multi_bars", "多柱状图", build_multibar, apply_multibar},
    {"dual_line", "双折线", build_dual, apply_dual},
    {"big_stat", "主指标卡", build_big, apply_big},
    {"spark_row", "迷你曲线排", build_sparkrow, apply_sparkrow},
    {"text_badge", "文本徽章", build_text, apply_text},
};

static const mini_entry_t *find_entry(const char *type)
{
    if (!type) return NULL;
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        if (strcmp(s_entries[i].type, type) == 0) return &s_entries[i];
    }
    return NULL;
}

bool ui_mini_is_type(const char *type) { return find_entry(type) != NULL; }

const char *ui_mini_label(const char *type)
{
    const mini_entry_t *e = find_entry(type);
    return e ? e->label : "组件";
}

/* ---------------- 公共 API ---------------- */

ui_mini_t *ui_mini_create(lv_obj_t *parent, int body_w, int body_h,
                          const ui_mini_spec_t *spec, int gw, int gh,
                          const ui_mini_bind_t *binds, int n_binds,
                          const ui_mini_row_t *rows, int n_rows)
{
    const mini_entry_t *e = find_entry(spec ? spec->type : NULL);
    if (!e || !parent) return NULL;
    ui_mini_t *m = calloc(1, sizeof(ui_mini_t));
    if (!m) return NULL;
    m->parent = parent;
    m->gw = gw;
    m->gh = gh;
    m->spec = *spec;
    snprintf(m->type, sizeof(m->type), "%s", spec->type);
    m->body_w = body_w;
    m->body_h = body_h;
    if (n_binds > UI_MINI_MAX_BINDS) n_binds = UI_MINI_MAX_BINDS;
    if (n_rows > UI_MINI_MAX_ROWS) n_rows = UI_MINI_MAX_ROWS;
    for (int i = 0; i < n_binds; i++) m->binds[i] = binds[i];
    m->n_binds = n_binds;
    for (int i = 0; i < n_rows; i++) m->rows[i] = rows[i];
    m->n_rows = n_rows;

    e->build(m);
    /* 让子对象 coords 立即生效：后续 apply 与绘制都依赖真实几何 */
    lv_obj_update_layout(parent);
    anim_register(m);
    return m;
}

void ui_mini_apply(ui_mini_t *m)
{
    if (!m) return;
    const mini_entry_t *e = find_entry(m->type);
    if (e && e->apply) e->apply(m);
}

void ui_mini_destroy(ui_mini_t *m)
{
    if (!m) return;
    anim_unregister(m);
    free(m);
}
