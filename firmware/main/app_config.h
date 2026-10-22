/**
 * 运行时配置（存 NVS，配网页面写入）
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
    char server_url[160];      /* 形如 http://192.168.1.100:8000，末尾不带 / */
    char api_key[192];
    char provider[16];         /* auto / halogen / gufo / strata / lmstudio / ollama / openai */
    char host_agent[160];      /* 可选，形如 http://192.168.1.100:8001 */
    uint8_t theme;             /* 0=经典 1=STRIX 横屏 2=TUF 竖屏 */
    bool rot180;               /* 相对主题默认方向的 180° 翻转 */
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
