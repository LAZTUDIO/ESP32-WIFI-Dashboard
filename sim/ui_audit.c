/**
 * 无头渲染的视觉缺陷巡检。
 *
 * 只报告"机器可判定"的四类问题：
 *   1. 元素越出屏幕
 *   2. 同一父节点下可见子元素互相重叠
 *   3. 文字宽度超出父容器
 *   4. 文字与最近背景色对比度过低（< 3.0）
 */
#include "ui_audit.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "lvgl.h"

#define MAX_REPORT 24

typedef struct {
    int w, h;
    int bounds, overlap, overflow, contrast;
    int printed;
} audit_ctx_t;

static uint32_t color24(lv_color_t c)
{
    return lv_color_to_u32(c) & 0xFFFFFFu;
}

static double lum_channel(unsigned c)
{
    double v = (double)c / 255.0;
    return (v <= 0.03928) ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
    double r = lum_channel((rgb >> 16) & 0xFF);
    double g = lum_channel((rgb >> 8) & 0xFF);
    double b = lum_channel(rgb & 0xFF);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static double contrast_ratio(uint32_t fg, uint32_t bg)
{
    double a = luminance(fg);
    double b = luminance(bg);
    if (a < b) { double t = a; a = b; b = t; }
    return (a + 0.05) / (b + 0.05);
}

static bool visible(const lv_obj_t * o)
{
    if (!o) return false;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return (a.x2 > a.x1) && (a.y2 > a.y1);
}

static uint32_t effective_bg(const lv_obj_t * o)
{
    const lv_obj_t * p = o;
    while (p) {
        if (lv_obj_get_style_bg_opa(p, LV_PART_MAIN) >= LV_OPA_50) {
            return color24(lv_obj_get_style_bg_color(p, LV_PART_MAIN));
        }
        p = lv_obj_get_parent(p);
    }
    return 0x000000u;
}

static void report_obj(audit_ctx_t * c, const char * kind, lv_obj_t * o,
                       const char * detail)
{
    c->printed++;
    if (c->printed > MAX_REPORT) return;

    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const char * text = "";
    char tbuf[48];
    if (lv_obj_has_class(o, &lv_label_class)) {
        snprintf(tbuf, sizeof(tbuf), "%s", lv_label_get_text(o));
        text = tbuf;
    }
    printf("[audit] %-12s (%d,%d)-(%d,%d) %s%s%s\n", kind,
           (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
           text[0] ? "text=\"" : "", text, text[0] ? "\"" : "");
    if (detail && detail[0]) printf("        %s\n", detail);
}

static void walk(audit_ctx_t * c, lv_obj_t * parent)
{
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return;

    uint32_t n = lv_obj_get_child_count(parent);
    lv_area_t pa;
    lv_obj_get_coords(parent, &pa);
    int32_t pcw = pa.x2 - pa.x1 + 1;

    /* 同级重叠 */
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * a = lv_obj_get_child(parent, i);
        if (!visible(a)) continue;
        lv_area_t ra;
        lv_obj_get_coords(a, &ra);

        for (uint32_t j = i + 1; j < n; j++) {
            lv_obj_t * b = lv_obj_get_child(parent, j);
            if (!visible(b)) continue;
            lv_area_t rb;
            lv_obj_get_coords(b, &rb);
            int32_t ox = LV_MIN(ra.x2, rb.x2) - LV_MAX(ra.x1, rb.x1) + 1;
            int32_t oy = LV_MIN(ra.y2, rb.y2) - LV_MAX(ra.y1, rb.y1) + 1;
            if (ox > 0 && oy > 0 && ox * oy > 8) {
                c->overlap++;
                char d[128];
                snprintf(d, sizeof(d), "overlap=%dx%d with sibling #%u",
                         (int)ox, (int)oy, (unsigned)j);
                report_obj(c, "OVERLAP", a, d);
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * o = lv_obj_get_child(parent, i);
        if (!visible(o)) continue;

        lv_area_t a;
        lv_obj_get_coords(o, &a);

        /* 越界 */
        if (a.x1 < 0 || a.y1 < 0 || a.x2 >= c->w || a.y2 >= c->h) {
            c->bounds++;
            char d[96];
            snprintf(d, sizeof(d), "screen=%dx%d", c->w, c->h);
            report_obj(c, "OUT-OF-BOUNDS", o, d);
        }

        /* 文字溢出：仅对 label 检查，且宽高都是显式约束的才可靠 */
        if (lv_obj_has_class(o, &lv_label_class)) {
            int32_t lw = lv_obj_get_width(o);
            if (lw > pcw + 2) {
                c->overflow++;
                char d[96];
                snprintf(d, sizeof(d), "label w=%d parent w=%d",
                         (int)lw, (int)pcw);
                report_obj(c, "TEXT-OVERFLOW", o, d);
            }

            /* 对比度 */
            lv_color_t tcol = lv_obj_get_style_text_color(o, LV_PART_MAIN);
            uint32_t fg = color24(tcol);
            uint32_t bg = effective_bg(o);
            double cr = contrast_ratio(fg, bg);
            if (cr < 3.0) {
                c->contrast++;
                char d[96];
                snprintf(d, sizeof(d), "contrast=%.2f fg=#%06X bg=#%06X",
                         cr, (unsigned)fg, (unsigned)bg);
                report_obj(c, "LOW-CONTRAST", o, d);
            }
        }

        walk(c, o);
    }
}

void ui_audit_run(int w, int h)
{
    audit_ctx_t c = { .w = w, .h = h };

    lv_obj_t * scr = lv_screen_active();
    if (scr && !lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN)) walk(&c, scr);
    lv_obj_t * top = lv_layer_top();
    if (top) walk(&c, top);

    printf("[audit] summary %dx%d: bounds=%d overlap=%d overflow=%d "
           "low_contrast=%d\n", w, h, c.bounds, c.overlap, c.overflow,
           c.contrast);
}
