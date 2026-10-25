/**
 * PC 模拟器：
 *   ./dash_sim --font <binfont> <out.png> ["示例文字"]   字体探针
 *   ./dash_sim --classic <theme> <out.png>               传统框架主题预览
 *              （theme = instrument|hud|mecha|stealth|arcade|drone|scope|
 *                heatsink|loadout|armor|sweep，480×320，内置演示数据）
 *
 * 小组件布局的配色预览请直接用 Web 编辑器（http://<server>:8080/），
 * 那里与服务端 theme_tokens 同源，所见即所得。
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "png_writer.h"
#include "dash_metric_store.h"
#include "ui_classic.h"

#define PROBE_W 320
#define PROBE_H 140
#define CLASSIC_W 480
#define CLASSIC_H 320

/* 帧缓冲按最大模式（传统框架 480×320）分配 */
static uint8_t g_fb[CLASSIC_W * CLASSIC_H * 2];

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    (void)area;
    (void)px_map;
    /* DIRECT 模式下 g_fb 已经是最终画面，无需拷贝 */
    lv_display_flush_ready(disp);
}

/* 传统框架演示数据：覆盖两套主题用到的全部指标 */
static void seed_classic_metrics(void)
{
    dash_metric_store_init();
    dash_metric_store_set_string("llm.model", "Qwen3-32B", true);
    dash_metric_store_set_bool("llm.online", true, true);
    dash_metric_store_set_bool("llm.busy", true, true);
    dash_metric_store_set_string("llm.phase", "DECODE", true);
    dash_metric_store_set_bool("tts.loaded", true, true);
    dash_metric_store_set_bool("embed.loaded", false, true);
    dash_metric_store_set_number("host.gpu_util_pct", 78.0, true);
    dash_metric_store_set_number("host.cpu_pct", 34.0, true);
    dash_metric_store_set_number("npu.util_pct", 52.0, true);
    dash_metric_store_set_number("host.ram_used_mb", 12.4 * 1024.0, true);
    dash_metric_store_set_number("host.ram_total_mb", 32.0 * 1024.0, true);
    dash_metric_store_set_number("host.disk_free_gb", 512.0, true);
    dash_metric_store_set_number("host.disk_total_gb", 2000.0, true);
    dash_metric_store_set_number("llm.kv_pct", 61.0, true);
    dash_metric_store_set_number("llm.kv_used_tokens", 148000.0, true);
    dash_metric_store_set_number("llm.kv_pool_positions", 245760.0, true);
    dash_metric_store_set_number("llm.slots", 4.0, true);
    dash_metric_store_set_number("llm.requests_deferred", 2.0, true);
    dash_metric_store_set_number("host.gpu_temp_c", 68.0, true);
    dash_metric_store_set_number("host.gpu_power_w", 312.0, true);
    /* H02-H10 主题额外使用 */
    dash_metric_store_set_number("llm.decode_tps", 42.3, true);
    dash_metric_store_set_number("llm.prefill_tps", 1860.0, true);
    dash_metric_store_set_number("llm.decode_avg", 38.1, true);
    dash_metric_store_set_number("llm.decode_peak", 58.9, true);
    dash_metric_store_set_number("host.gpu_vram_used_mb", 18.2 * 1024.0, true);
    dash_metric_store_set_number("host.gpu_vram_total_mb", 24.0 * 1024.0,
                                 true);
    dash_metric_store_set_number("llm.context_len", 32768.0, true);
}

static int run_classic(const char *theme, const char *out)
{
    lv_init();
    lv_display_t * disp = lv_display_create(CLASSIC_W, CLASSIC_H);
    if (!disp) {
        fprintf(stderr, "display create failed\n");
        return 1;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_fb, NULL,
                           (size_t)CLASSIC_W * CLASSIC_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    seed_classic_metrics();
    ui_classic_t * c = ui_classic_build(theme, CLASSIC_W, CLASSIC_H);
    if (!c) {
        fprintf(stderr, "classic theme build failed: %s\n", theme);
        return 1;
    }
    lv_screen_load(ui_classic_screen(c));
    lv_refr_now(disp);
    int rc = png_write_rgb565(out, (const uint16_t *)g_fb, CLASSIC_W,
                              CLASSIC_H);
    printf("classic theme: %s -> %s\n", theme, out);
    ui_classic_destroy(c);
    return rc;
}

int main(int argc, char ** argv)
{
    if (argc >= 4 && strcmp(argv[1], "--classic") == 0) {
        return run_classic(argv[2], argv[3]);
    }

    if (argc < 4 || strcmp(argv[1], "--font") != 0) {
        fprintf(stderr,
                "usage: %s --font <binfont> <out.png> [\"text\"]\n"
                "       %s --classic <theme> <out.png>\n"
                "         theme = instrument|hud|mecha|stealth|arcade|drone\n"
                "                 |scope|heatsink|loadout|armor|sweep\n",
                argv[0], argv[0]);
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
