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
#include "ui.h"

static const char *TAG = "dbg";

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
    const uint8_t *rgba = (const uint8_t *)snap->data;
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

    printf("%s fps=%.1f frames=%lu heap=%u min_heap=%u psram=%u "
           "wifi=%s ip=%s rssi=%d\n",
           prefix, fps, (unsigned long)frames,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           wifi_mgr_sta_connected() ? "up" : "down", ip, (int)wifi_mgr_rssi());
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

/* c <服务地址>|<密钥>  —— 串口配 API 服务 */
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
        snprintf(cfg.server_url, sizeof(cfg.server_url), "%s",
                 trim_tail(trim_head(arg)));
    } else {
        printf("[W] dbg: 用法 c <url>|<key>\n");
        return;
    }
    printf("[I] dbg: server='%s' key_len=%u\n", cfg.server_url,
           (unsigned)strlen(cfg.api_key));
    save_and_reboot(&cfg);
}

/* ============================ 命令解析 ============================ */

static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\r' || *line == '\n') line++;
    if (*line == '\0') return;

    switch (*line) {
    case 's': cmd_snapshot(); break;
    case 'h': print_stats("STATS"); break;
    case 'w': cmd_wifi(line + 1); break;
    case 'c': cmd_server(line + 1); break;
    case 'p':
        if (lvgl_port_lock(300)) {
            dashboard_ui_set_page(atoi(line + 1));
            lvgl_port_unlock();
        }
        break;
    case '?':
        printf("commands: s=snapshot  h=stats  p <0-3>=page  "
               "w <ssid>|<pass>=wifi  c <url>|<key>=server\n");
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
