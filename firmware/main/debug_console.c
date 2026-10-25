/**
 * 串口调试控制台 —— 让 agent 能自己抓画面、看状态
 *
 * 命令（每行一条，走 UART stdin，idf.py monitor 可直接输入）：
 *   s      抓一帧，PNG → base64 输出到串口
 *   h      打印一次 FPS / 内存 / Wi-Fi
 *   ?      帮助
 *
 * base64 而不是裸二进制：UART 的 VFS 层可能做 CR/LF 转换，裸二进制会被破坏。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "libs/lodepng/lodepng.h"

#include "debug_console.h"
#include "display.h"
#include "wifi_mgr.h"
#include "app_config.h"
#include "ui_dynamic.h"
#include "dash_metric_store.h"
#include "app_actions.h"

static const char *TAG = "dbg";
static QueueHandle_t s_perf_q;

#define B64_LINE 76

/* 抓帧期间禁止其它任务往 stdout / 日志里写字，否则 base64 会被插断 */
static volatile bool s_dump_busy;

/* ============================ base64 ============================ */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_dump(const uint8_t *data, size_t len)
{
    size_t i = 0;
    int col = 0;
    while (i + 2 < len) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        putchar(B64[(v >> 18) & 63]);
        putchar(B64[(v >> 12) & 63]);
        putchar(B64[(v >> 6) & 63]);
        putchar(B64[v & 63]);
        i += 3;
        col += 4;
        if (col >= B64_LINE) { putchar('\n'); col = 0; }
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        putchar(B64[(v >> 18) & 63]);
        putchar(B64[(v >> 12) & 63]);
        putchar(i + 1 < len ? B64[(v >> 6) & 63] : '=');
        putchar('=');
    }
    if (col != 0) putchar('\n');
}

/* ============================ 抓帧 ============================ */

/** 把 top 层（配网浮层 / toast）按 alpha 叠到屏幕快照上 */
static void blend_top(uint8_t *dst, size_t dstride, const uint8_t *src,
                      size_t sstride, int w, int h)
{
    for (int y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * dstride;
        const uint8_t *s = src + (size_t)y * sstride;
        for (int x = 0; x < w; x++, d += 4, s += 4) {
            const uint8_t a = s[3];
            if (a == 0) continue;
            if (a == 255) {
                memcpy(d, s, 4);
                continue;
            }
            for (int c = 0; c < 3; c++) {
                d[c] = (uint8_t)((s[c] * a + d[c] * (255 - a)) / 255);
            }
            d[3] = 255;
        }
    }
}

static void cmd_snapshot(void)
{
    if (!lvgl_port_lock(100)) {
        printf("[E] dbg: lvgl lock timeout, snapshot skipped\n");
        return;
    }

    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(),
                                           LV_COLOR_FORMAT_ARGB8888);
    ESP_LOGI(TAG, "snapshot=%p", (void *)snap);
    if (!snap) {
        lvgl_port_unlock();
        printf("[E] dbg: snapshot failed (LV_USE_SNAPSHOT 是否开启?)\n");
        return;
    }

    const int w = snap->header.w;
    const int h = snap->header.h;
    const size_t stride = snap->header.stride;

    /* lv_snapshot_take(screen) 只拍 screen 自己的子树，
       配网浮层和 toast 在 top layer 上，需要手动叠上去 */
    lv_obj_t *top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) > 0) {
        lv_draw_buf_t *ts = lv_snapshot_take(top, LV_COLOR_FORMAT_ARGB8888);
        if (ts) {
            int tw = ts->header.w < w ? ts->header.w : w;
            int th = ts->header.h < h ? ts->header.h : h;
            blend_top(snap->data, stride, ts->data, ts->header.stride, tw, th);
            lv_draw_buf_destroy(ts);
        }
    }

    uint8_t *packed = NULL;
    uint8_t *rgba = (uint8_t *)snap->data;
    /* LVGL 的 ARGB8888 在小端机上按 B,G,R,A 存储，lodepng_encode32 需要 R,G,B,A；
       不转换会让截图红蓝互换，误判面板颜色。 */
    for (int y = 0; y < h; y++) {
        uint8_t *row = rgba + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 4];
            row[x * 4] = row[x * 4 + 2];
            row[x * 4 + 2] = b;
        }
    }
    if (stride != (size_t)w * 4) {
        packed = heap_caps_malloc((size_t)w * h * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (packed) {
            for (int y = 0; y < h; y++) {
                memcpy(packed + (size_t)y * w * 4, rgba + (size_t)y * stride,
                       (size_t)w * 4);
            }
            rgba = packed;
        }
    }

    uint8_t *png = NULL;
    size_t png_len = 0;
    ESP_LOGI(TAG, "encoding %dx%d stride=%u", w, h, (unsigned)stride);
    unsigned err = lodepng_encode32(&png, &png_len, rgba, w, h);
    ESP_LOGI(TAG, "encoded png=%p len=%u err=%u", (void *)png,
             (unsigned)png_len, err);

    if (packed) heap_caps_free(packed);
    lv_draw_buf_destroy(snap);
    lvgl_port_unlock();

    if (err || !png) {
        printf("[E] dbg: png encode failed (%u)\n", err);
        if (png) lv_free(png);
        return;
    }

    s_dump_busy = true;
    esp_log_level_set("*", ESP_LOG_NONE);

    printf("---SNAP-BEGIN w=%d h=%d bytes=%zu---\n", w, h, png_len);
    base64_dump(png, png_len);
    printf("---SNAP-END---\n");
    fflush(stdout);

    esp_log_level_set("*", ESP_LOG_INFO);
    s_dump_busy = false;

    lv_free(png);
}

/* ============================ 状态打印 ============================ */

static void print_stats(const char *prefix)
{
    static uint32_t last_frames = 0;
    static int64_t last_us = 0;

    int64_t now = esp_timer_get_time();
    uint32_t frames = display_get_frame_count();
    float fps = 0.0f;
    if (last_us != 0 && now > last_us) {
        fps = (float)(frames - last_frames) * 1e6f / (float)(now - last_us);
    }
    last_frames = frames;
    last_us = now;

    char ip[16] = "--";
    wifi_mgr_sta_ip(ip, sizeof(ip));

    printf("%s fps=%.1f frames=%lu heap=%u min_heap=%u psram=%u int_free=%u "
           "int_max=%u wifi=%s ip=%s rssi=%d\n",
           prefix, fps, (unsigned long)frames,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           wifi_mgr_sta_connected() ? "up" : "down", ip, (int)wifi_mgr_rssi());
    if (ui_dynamic_active()) {
        ui_dynamic_perf_t p;
        ui_dynamic_perf(&p);
        printf("  ui: apply last/avg/max=%lld/%lld/%lld us n=%lu\n",
               (long long)p.apply_last_us, (long long)p.apply_avg_us,
               (long long)p.apply_max_us, (unsigned long)p.apply_count);
    }
    fflush(stdout);
}

/* ============================ 串口配网 ============================ */

static char *trim_tail(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

static char *trim_head(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void save_and_reboot(const app_config_t *cfg)
{
    if (!config_save(cfg)) {
        printf("[E] dbg: 保存失败\n");
        fflush(stdout);
        return;
    }
    printf("[I] dbg: 已保存，重启中…\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

/* w <ssid>|<密码>  —— 串口配 Wi-Fi */
static void cmd_wifi(char *arg)
{
    char *bar = strchr(arg, '|');
    if (!bar) {
        printf("[W] dbg: 用法 w <ssid>|<password>\n");
        return;
    }
    *bar = '\0';

    app_config_t cfg;
    config_load(&cfg);
    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", trim_tail(trim_head(arg)));
    snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", trim_tail(bar + 1));
    printf("[I] dbg: wifi ssid='%s' pass_len=%u\n", cfg.wifi_ssid,
           (unsigned)strlen(cfg.wifi_pass));
    save_and_reboot(&cfg);
}

/* c <服务地址>|<密钥>  —— 串口配传统 API */
static void cmd_server(char *arg)
{
    char *bar = strchr(arg, '|');
    app_config_t cfg;
    config_load(&cfg);
    if (bar) {
        *bar = '\0';
        snprintf(cfg.server_url, sizeof(cfg.server_url), "%s",
                 trim_tail(trim_head(arg)));
        snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", trim_tail(bar + 1));
    } else if (*arg) {
        if (strcmp(trim_tail(trim_head(arg)), "-") == 0) {
            cfg.server_url[0] = '\0';
        } else {
            snprintf(cfg.server_url, sizeof(cfg.server_url), "%s",
                     trim_tail(trim_head(arg)));
        }
    } else {
        printf("[W] dbg: 用法 c <url>|<key>\n");
        return;
    }
    config_normalize(&cfg);
    printf("[I] dbg: api='%s' key_len=%u\n", cfg.server_url,
           (unsigned)strlen(cfg.api_key));
    save_and_reboot(&cfg);
}

/* o <主机监控/面板地址>  —— 串口配 host_agent（可填 8800 面板） */
static void cmd_host(char *arg)
{
    app_config_t cfg;
    config_load(&cfg);
    if (!*arg) {
        printf("[W] dbg: 用法 o <url>  例如 o http://192.168.1.100:8800；o - 清空\n");
        return;
    }
    if (strcmp(trim_tail(trim_head(arg)), "-") == 0) {
        cfg.host_agent[0] = '\0';
    } else {
        snprintf(cfg.host_agent, sizeof(cfg.host_agent), "%s",
                 trim_tail(trim_head(arg)));
    }
    config_normalize(&cfg);
    printf("[I] dbg: host_agent='%s'\n", cfg.host_agent);
    save_and_reboot(&cfg);
}

/* d <url>  —— 配置动态看板服务端 */
static void cmd_dashboard(char *arg)
{
    app_config_t cfg;
    config_load(&cfg);
    if (!*arg) {
        printf("[W] dbg: 用法 d <url>  例如 d http://192.168.1.100:8080；d - 清空\n");
        return;
    }
    if (strcmp(trim_tail(trim_head(arg)), "-") == 0) {
        cfg.dashboard_url[0] = '\0';
    } else {
        snprintf(cfg.dashboard_url, sizeof(cfg.dashboard_url), "%s",
                 trim_tail(trim_head(arg)));
    }
    config_normalize(&cfg);
    printf("[I] dbg: dashboard='%s'\n", cfg.dashboard_url);
    save_and_reboot(&cfg);
}

/* ============================ 动画帧率基准 ============================ */

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void anim_color_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_color((lv_obj_t *)var,
                              lv_color_hsv_to_rgb((uint16_t)(v % 360), 80, 60), 0);
}

static void anim_fps_bench(bool fullscreen)
{
    lv_obj_t *o = NULL;

    if (lvgl_port_lock(1500)) {
        o = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_color(o, lv_color_hex(0xFF2A45), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_time(&a, 2000);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

        if (fullscreen) {
            lv_display_t *d = lv_display_get_default();
            lv_obj_set_pos(o, 0, 0);
            lv_obj_set_size(o, lv_display_get_horizontal_resolution(d),
                            lv_display_get_vertical_resolution(d));
            lv_anim_set_exec_cb(&a, anim_color_cb);
            lv_anim_set_values(&a, 0, 359);
        } else {
            lv_obj_set_size(o, 64, 64);
            lv_obj_set_pos(o, 0, 80);
            lv_anim_set_exec_cb(&a, anim_x_cb);
            lv_anim_set_values(&a, 0, 400);
        }
        lv_anim_start(&a);
        lvgl_port_unlock();
    }

    uint32_t f0 = display_get_frame_count();
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(5000));
    int64_t dt = esp_timer_get_time() - t0;
    uint32_t f1 = display_get_frame_count();

    if (lvgl_port_lock(1500)) {
        if (o) {
            lv_anim_del(o, NULL);
            lv_obj_delete(o);
        }
        lvgl_port_unlock();
    }

    printf("[I] dbg: %s anim fps=%.1f frames=%u dt=%lldms\n",
           fullscreen ? "fullscreen" : "small",
           (double)(f1 - f0) * 1e6 / (double)dt, (unsigned)(f1 - f0),
           (long long)(dt / 1000));
    fflush(stdout);
}

static void run_render_cmd(char cmd)
{
    if (cmd == 'b') {
        if (lvgl_port_lock(2000)) {
            int64_t t0 = esp_timer_get_time();
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(lv_display_get_default());
            int64_t dt = (esp_timer_get_time() - t0) / 1000;
            printf("[I] dbg: full refresh %lld ms (%dx%d)\n", (long long)dt,
                   (int)lv_display_get_horizontal_resolution(lv_display_get_default()),
                   (int)lv_display_get_vertical_resolution(lv_display_get_default()));
            fflush(stdout);
            lvgl_port_unlock();
        }
    } else if (cmd == 'B') {
        if (lvgl_port_lock(2000)) {
            lv_display_t *d = lv_display_get_default();
            int64_t t0;

            display_flush_bypass(true);
            t0 = esp_timer_get_time();
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(d);
            int64_t render_ms = (esp_timer_get_time() - t0) / 1000;
            display_flush_bypass(false);

            t0 = esp_timer_get_time();
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(d);
            int64_t full_ms = (esp_timer_get_time() - t0) / 1000;

            printf("[I] dbg: render=%lld ms  full=%lld ms  transfer~%lld ms\n",
                   (long long)render_ms, (long long)full_ms,
                   (long long)(full_ms - render_ms));
            fflush(stdout);
            lvgl_port_unlock();
        }
    } else if (cmd == 'C') {
        if (lvgl_port_lock(2000)) {
            lv_display_t *d = lv_display_get_default();
            lv_obj_t *prev = lv_screen_active();
            lv_obj_t *blank = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(blank, lv_color_hex(0x101010), 0);
            lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
            lv_screen_load(blank);

            int64_t t0 = esp_timer_get_time();
            lv_obj_invalidate(blank);
            lv_refr_now(d);
            int64_t full_ms = (esp_timer_get_time() - t0) / 1000;
            printf("[I] dbg: blank full=%lld ms\n", (long long)full_ms);
            fflush(stdout);

            lv_screen_load(prev);
            lv_obj_delete(blank);
            lvgl_port_unlock();
        }
    }
}

static void perf_task(void *arg)
{
    (void)arg;
    char cmd;
    while (1) {
        if (xQueueReceive(s_perf_q, &cmd, portMAX_DELAY) == pdTRUE) {
            run_render_cmd(cmd);
        }
    }
}

static bool ensure_perf_task(void)
{
    if (s_perf_q) return true;
    s_perf_q = xQueueCreate(2, sizeof(char));
    if (!s_perf_q) return false;
    if (xTaskCreateWithCaps(perf_task, "dbgperf", 12288, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_perf_q);
        s_perf_q = NULL;
        printf("[E] dbg: perf task create failed (internal free=%u largest=%u)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

/* ============================ 命令解析 ============================ */

static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\r' || *line == '\n') line++;
    if (*line == '\0') return;

    switch (*line) {
    case 's': cmd_snapshot(); break;
    case 'h': print_stats("STATS"); break;
    case 'b':
    case 'B':
    case 'C':
        if (ensure_perf_task()) {
            char cmd = *line;
            if (xQueueSend(s_perf_q, &cmd, 0) == pdTRUE) {
                printf("[I] dbg: render bench queued (%c)\n", cmd);
                fflush(stdout);
                break;
            }
        }
        printf("[E] dbg: render bench unavailable\n");
        break;
    case 'a': anim_fps_bench(false); break;
    case 'A': anim_fps_bench(true); break;
    case 'w': cmd_wifi(line + 1); break;
    case 'c': cmd_server(line + 1); break;
    case 'o': cmd_host(line + 1); break;
    case 'd': cmd_dashboard(line + 1); break;
    case 'm':
        dash_metric_store_dump();
        break;
    case 'P':
        printf("[I] dbg: requesting provisioning mode\n");
        app_request_provisioning();
        break;
    case '?':
        printf("commands: s=snapshot  h=stats  b=bench refresh  B=render/transfer  "
               "a=small anim fps  A=full anim fps  "
               "w <ssid>|<pass>=wifi  c <url>|<key>=api  o <url>=host  "
               "d <url>=dashboard  m=dump metrics  P=provision\n");
        break;
    default:
        printf("[W] dbg: unknown command '%c'\n", *line);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[256];
    printf("[I] dbg: console ready (type ? for help)\n");
    fflush(stdout);

    while (1) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_line(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_dump_busy) print_stats("PERF");
    }
}

/* ============================ 对外入口 ============================ */

void debug_console_start(void)
{
    xTaskCreate(console_task, "dbgcon", 8192, NULL, 3, NULL);
    xTaskCreate(stats_task, "dbgstat", 3072, NULL, 2, NULL);
}
