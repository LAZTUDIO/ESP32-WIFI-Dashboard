/**
 * 字体探针：把服务端下发的 LVGL binfont 渲染成 PNG，验证子集格式与字形。
 *
 *   ./dash_sim --font <binfont> <out.png> ["示例文字"]
 *
 * 页面配色预览请直接用 Web 编辑器（http://<server>:8080/），
 * 那里与服务端 theme_tokens 同源，所见即所得。
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "png_writer.h"

#define PROBE_W 320
#define PROBE_H 140

static uint8_t g_fb[PROBE_W * PROBE_H * 2];

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    (void)area;
    (void)px_map;
    /* DIRECT 模式下 g_fb 已经是最终画面，无需拷贝 */
    lv_display_flush_ready(disp);
}

int main(int argc, char ** argv)
{
    if (argc < 4 || strcmp(argv[1], "--font") != 0) {
        fprintf(stderr,
                "usage: %s --font <binfont> <out.png> [\"text\"]\n"
                "配色预览请打开 Web 编辑器（服务端 /）\n",
                argv[0]);
        return 2;
    }

    const char * bin = argv[2];
    const char * out = argv[3];
    const char * text = (argc > 4) ? argv[4] : "温度曲线 熵排队测试";

    lv_init();

    lv_display_t * disp = lv_display_create(PROBE_W, PROBE_H);
    if (!disp) {
        fprintf(stderr, "display create failed\n");
        return 1;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_fb, NULL,
                           (size_t)PROBE_W * PROBE_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    char path[512];
    snprintf(path, sizeof(path), "A:%s", bin);
    lv_font_t * font = lv_binfont_create(path);
    if (!font) {
        fprintf(stderr, "binfont load failed: %s\n", path);
        return 1;
    }

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t * lbl = lv_label_create(scr);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(lbl, 8, 12);
    lv_screen_load(scr);
    lv_refr_now(disp);

    int rc = png_write_rgb565(out, (const uint16_t *)g_fb, PROBE_W, PROBE_H);
    printf("font probe: %s -> %s (%s)\n", bin, out, text);
    lv_binfont_destroy(font);
    return rc;
}
