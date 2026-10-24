#pragma once

#include <stdbool.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 线程安全地复制当前运行时配置。 */
void app_runtime_snapshot(app_config_t *out);

/** 保存配置并热应用（API 设置、服务端地址、名称、旋转）。
 *  由设备管理 HTTP 的 /api/v1/config 调用。 */
bool app_runtime_apply_config(const app_config_t *cfg);

/** 服务端 WebSocket 是否已连接（用于网络信息页和 /info）。 */
bool app_runtime_server_connected(void);
void app_runtime_set_server_connected(bool connected);

/** 显示/隐藏“网络已连接 + IP”浮层。 */
void app_runtime_show_network(bool show);

#ifdef __cplusplus
}
#endif
