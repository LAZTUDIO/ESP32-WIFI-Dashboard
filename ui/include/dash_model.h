/**
 * 仪表盘数据模型 —— 与平台无关，PC 模拟器与 ESP32 固件共用。
 */
#ifndef DASH_MODEL_H
#define DASH_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_HISTORY_LEN 60

typedef enum {
    DASH_PROV_UNKNOWN = 0,
    DASH_PROV_HALOGEN,
    DASH_PROV_GUFO,
    DASH_PROV_STRATA,
    DASH_PROV_LMSTUDIO,
    DASH_PROV_OLLAMA,
    DASH_PROV_OPENAI,
    DASH_PROV_PANEL,            /* halogen-panel 8800 /api/status */
} dash_provider_t;

typedef struct {
    bool online;
    bool busy;
    bool stale;
    bool tps_live;              /* true = 当前采样是实时值，false = 最近一次完成的请求 */

    dash_provider_t provider;
    char provider_name[20];
    char model[64];
    char version[24];
    char endpoint[96];
    char host_agent[96];
    char api_key_mask[24];      /* 已脱敏的密钥，供设置页显示 */

    /* 网络与设备 */
    char wifi_ssid[33];
    char wifi_ip[16];
    int8_t wifi_rssi;
    int8_t device_temp_c;
    uint32_t free_heap_kb;

    bool has_slots;
    uint16_t slots;
    uint32_t ctx_len;

    bool has_concurrency;
    uint16_t in_flight;
    bool has_queue;
    uint16_t queued;

    bool has_kv;
    float kv_pct;
    uint32_t kv_used;
    uint32_t kv_total;

    bool has_tps;
    float tps_dec;              /* 当前 / 最近请求的解码 tok/s */
    float tps_pre;              /* 当前 / 最近请求的预填充 tok/s */
    float tps_avg;              /* 引擎累计均值 */
    float tps_avg_pre;
    float tps_peak;             /* 本次开机观察到峰值 */
    float tps_peak_pre;

    bool has_usage;
    uint64_t tokens_in;
    uint64_t tokens_out;
    uint64_t tokens_cached;

    bool has_requests;
    uint64_t requests_total;

    bool has_mtp;
    float mtp_pct;

    bool has_cache;
    float cache_hit_pct;
    uint64_t cache_bytes;
    uint32_t cache_entries;

    bool has_power;
    float power_w;
    bool has_temp;
    float temp_c;
    bool has_gpu;
    float gpu_pct;
    bool has_vram;
    uint32_t vram_used_mb;
    uint32_t vram_total_mb;
    bool has_ram;
    uint32_t ram_used_mb;
    uint32_t ram_total_mb;

    bool has_host_uptime;
    uint32_t host_uptime_s;
    bool has_model_uptime;
    uint32_t model_uptime_s;
    bool has_engine_time;
    uint32_t engine_time_s;

    uint32_t device_uptime_s;
    uint32_t last_ok_ms;
} dash_metrics_t;

typedef struct {
    float tps_dec[DASH_HISTORY_LEN];
    float tps_pre[DASH_HISTORY_LEN];
    float kv[DASH_HISTORY_LEN];
    float conc[DASH_HISTORY_LEN];
    float power[DASH_HISTORY_LEN];
    float temp[DASH_HISTORY_LEN];
    uint16_t count;             /* 有效样本数，0..DASH_HISTORY_LEN */
} dash_history_t;

#ifdef __cplusplus
}
#endif

#endif /* DASH_MODEL_H */
