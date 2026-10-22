/**
 * 手机配网门户：SoftAP + HTTP 配置页 + DNS 劫持（Captive Portal）
 */
#ifndef PORTAL_H
#define PORTAL_H

#include <stdbool.h>

#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动热点 + HTTP 配置页 + DNS 劫持；cfg 用于预填表单 */
esp_err_t portal_start(const app_config_t *cfg);
/** 关闭配置页与热点（联网成功后调用） */
void portal_stop(void);
bool portal_running(void);

#ifdef __cplusplus
}
#endif

#endif /* PORTAL_H */
