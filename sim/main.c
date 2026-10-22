/**
 * 无头模拟器：LVGL 渲染到内存帧缓冲，直接输出 PNG。
 *
 * 没有 SDL / 没有窗口，因此可以在任何环境（含 CI）里跑，
 * 让 agent 能自己截图、自己判断视觉效果。
 *
 *   ./dash_sim out/dashboard.png [page] [scenario] [theme] [rot180]
 *     page     0=概览 1=曲线 2=系统 3=设置 4=配网浮层
 *     scenario live(默认) | offline | empty
 *     theme    0=经典 320×480  1=STRIX 480×320  2=TUF 320×480
 *     rot180   0=正装 1=倒装（出图翻转，模拟屏幕 180°）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lvgl.h"
#include "ui.h"
#include "png_writer.h"
#include "ui_audit.h"

/* 两种朝向字节数相同：320×480 或 480×320，均 300KB */
static uint8_t g_fb[UI_STRIX_W * UI_STRIX_H * 2];

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    (void)area;
    (void)px_map;
    /* DIRECT 模式下 g_fb 已经是最终画面，无需拷贝 */
    lv_display_flush_ready(disp);
}

/* ============================== 假数据 ============================== */

static void mock_base(dash_metrics_t * m)
{
    memset(m, 0, sizeof(*m));

    m->online = true;
    m->provider = DASH_PROV_HALOGEN;
    snprintf(m->provider_name, sizeof(m->provider_name), "halogen");
    snprintf(m->model, sizeof(m->model), "halogen-qwen3.8-flash-next");
    snprintf(m->version, sizeof(m->version), "0.18.3");
    snprintf(m->endpoint, sizeof(m->endpoint), "192.168.1.100:8000");
    snprintf(m->host_agent, sizeof(m->host_agent), "192.168.1.100:8001");
    snprintf(m->api_key_mask, sizeof(m->api_key_mask), "sk-demo-…demo");

    snprintf(m->wifi_ssid, sizeof(m->wifi_ssid), "Demo-WiFi");
    snprintf(m->wifi_ip, sizeof(m->wifi_ip), "192.168.1.42");
    m->wifi_rssi = -52;
    m->device_temp_c = 47;
    m->free_heap_kb = 148;

    m->has_slots = true;
    m->slots = 3;
    m->ctx_len = 262144;

    m->has_concurrency = true;
    m->in_flight = 1;
    m->has_queue = true;
    m->queued = 2;

    m->has_kv = true;
    m->kv_total = 786432;
    m->has_usage = true;
    m->tokens_in = 1284000;
    m->tokens_out = 926400;
    m->tokens_cached = 1120000;
    m->has_requests = true;
    m->requests_total = 3862;
    m->has_mtp = true;
    m->mtp_pct = 68.0f;

    m->has_cache = true;
    m->cache_hit_pct = 72.5f;
    m->cache_bytes = 512ULL * 1024 * 1024;
    m->cache_entries = 214;

    m->has_power = true;
    m->has_temp = true;
    m->has_gpu = true;
    m->has_vram = true;
    m->vram_used_mb = 19800;
    m->vram_total_mb = 24576;

    m->has_host_uptime = true;
    m->host_uptime_s = 3 * 86400 + 5 * 3600 + 12 * 60;
    m->has_model_uptime = true;
    m->model_uptime_s = 9 * 3600 + 42 * 60;
    m->has_engine_time = true;
    m->engine_time_s = 86400 + 3 * 3600;
    m->device_uptime_s = 2 * 3600 + 17 * 60;
    m->last_ok_ms = 120;
}

static void mock_tick(dash_metrics_t * m, int i, int n)
{
    float t = (float)i;
    float base = 46.0f + 9.0f * sinf(t * 0.23f) + 3.5f * sinf(t * 0.71f);

    m->busy = (i % 7) != 5;
    m->tps_live = (i >= n - 12);
    m->tps_dec = 6.0f + base;
    m->tps_pre = (i % 9 == 0) ? 620.0f + 90.0f * sinf(t) : 0.0f;
    m->tps_avg = 47.8f;
    m->tps_avg_pre = 410.0f;
    m->tps_peak = 74.2f;
    m->tps_peak_pre = 780.0f;
    m->has_tps = true;

    m->kv_pct = 38.0f + 24.0f * sinf(t * 0.09f) + 2.0f * sinf(t * 0.5f);
    m->kv_used = (uint32_t)(m->kv_total * m->kv_pct / 100.0f);

    m->in_flight = (uint16_t)(1 + (int)(2.4f * (0.5f + 0.5f * sinf(t * 0.4f))));
    m->queued = (uint16_t)(i % 11 < 4 ? (i % 4) : 0);

    m->power_w = 132.0f + 26.0f * sinf(t * 0.18f) + 2.0f * sinf(t * 1.3f);
    m->temp_c = 58.0f + 7.0f * sinf(t * 0.11f);
    m->gpu_pct = 74.0f + 12.0f * sinf(t * 0.31f);
    m->has_power = true;
    m->has_temp = true;
    m->has_gpu = true;
}

/* ============================== 入口 ============================== */

static void flip180(uint16_t * px, int w, int h)
{
    for (int y = 0; y < h / 2; y++) {
        uint16_t * a = px + (size_t)y * w;
        uint16_t * b = px + (size_t)(h - 1 - y) * w;
        for (int x = 0; x < w; x++) {
            uint16_t t = a[x];
            a[x] = b[w - 1 - x];
            b[w - 1 - x] = t;
        }
    }
    if (h & 1) {
        uint16_t * m = px + (size_t)(h / 2) * w;
        for (int x = 0; x < w / 2; x++) {
            uint16_t t = m[x];
            m[x] = m[w - 1 - x];
            m[w - 1 - x] = t;
        }
    }
}

int main(int argc, char ** argv)
{
    const char * out = (argc > 1) ? argv[1] : "out/dashboard.png";
    int page = (argc > 2) ? atoi(argv[2]) : 0;
    const char * scenario = (argc > 3) ? argv[3] : "live";
    int theme = (argc > 4) ? atoi(argv[4]) : 0;
    bool rot180 = (argc > 5) ? atoi(argv[5]) != 0 : false;
    if (theme < 0 || theme >= DASH_THEME_COUNT) theme = 0;

    int sw = (theme == DASH_THEME_STRIX) ? UI_STRIX_W : UI_SCREEN_W;
    int sh = (theme == DASH_THEME_STRIX) ? UI_STRIX_H : UI_SCREEN_H;

    lv_init();

    lv_display_t * disp = lv_display_create(sw, sh);
    if (!disp) {
        fprintf(stderr, "display create failed\n");
        return 1;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_fb, NULL, (size_t)sw * sh * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    dashboard_ui_set_prefs((dash_theme_t)theme, rot180);
    dashboard_ui_create();

    dash_metrics_t m;
    mock_base(&m);

    if (strcmp(scenario, "empty") == 0) {
        memset(&m, 0, sizeof(m));
        snprintf(m.wifi_ssid, sizeof(m.wifi_ssid), "Demo-WiFi");
        snprintf(m.wifi_ip, sizeof(m.wifi_ip), "192.168.1.42");
        m.wifi_rssi = -61;
        m.free_heap_kb = 152;
        m.device_temp_c = 44;
        m.online = false;
        m.stale = true;
    }

    const int n = 60;
    for (int i = 0; i < n; i++) {
        if (strcmp(scenario, "live") == 0) mock_tick(&m, i, n);
        else if (strcmp(scenario, "offline") == 0) {
            /* 保留历史曲线，但服务端已失联 */
            mock_tick(&m, i, n);
            if (i >= 40) m.online = false;
        }
        m.device_uptime_s++;
        dashboard_ui_apply(&m, true);
        lv_tick_inc(1000);
        lv_timer_handler();
    }

    if (page == 4) {
        dashboard_ui_show_provisioning(true, "ESP32-LLM-Setup", "llm123456",
                                       "192.168.4.1");
    } else {
        int maxpage = (theme == DASH_THEME_STRIX) ? 1 :
                      (theme == DASH_THEME_TUF) ? 2 : 3;
        dashboard_ui_set_page(page >= 0 && page <= maxpage ? page : 0);
    }

    /* 再跑几轮定时器，让布局/图表落到稳定帧 */
    for (int i = 0; i < 12; i++) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
    lv_refr_now(disp);

    ui_audit_run(sw, sh);

    if (rot180) flip180((uint16_t *)g_fb, sw, sh);

    int rc = png_write_rgb565(out, (const uint16_t *)g_fb, sw, sh);
    if (rc) {
        fprintf(stderr, "PNG write failed (%s)\n", out);
        return 1;
    }
    printf("rendered %dx%d page=%d scenario=%s theme=%d rot180=%d -> %s\n",
           sw, sh, page, scenario, theme, rot180 ? 1 : 0, out);
    return 0;
}
