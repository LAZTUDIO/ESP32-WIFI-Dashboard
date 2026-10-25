/**
 * 运行时配置（存 NVS，配网页面写入）
 *
 * 两路数据源可同时存在：
 *   - server_url：传统 LLM API（halogen / gufo / strata / LM Studio / Ollama / OpenAI）
 *   - dashboard_url：专用看板服务（下发布局/配色，并推送服务端指标）
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char device_id[24];        /* 由 MAC 派生，如 esp32-a1b2c3 */
    char device_name[32];      /* 服务端下发的备注名 */
    char server_url[160];      /* 传统 API 地址，末尾不带 / */
    char api_key[192];
    char provider[16];         /* auto / halogen / gufo / strata / lmstudio / ollama / openai / panel */
    char host_agent[160];      /* 可选主机监控，也可直接填 8800 面板 */
    char dashboard_url[160];   /* 看板服务端；由服务端按 IP 连接后下发 */
    char profile[16];          /* 屏幕方向：320x480 竖屏 / 480x320 横屏 */
    bool rot180;               /* 屏幕翻转（横屏时表示 270°） */
} app_config_t;

void config_defaults(app_config_t *cfg);
void config_load(app_config_t *cfg);
bool config_save(const app_config_t *cfg);

/** 去掉末尾多余的 '/'，让拼接路径简单一些 */
void config_normalize(app_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
