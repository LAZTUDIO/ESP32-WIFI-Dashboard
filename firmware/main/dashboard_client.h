#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动动态看板客户端：等待 Wi-Fi → 拉取服务端布局 → 建立 WS 订阅。
 * dashboard_url 形如 http://10.0.0.2:8080，空字符串表示不启用。
 */
void dashboard_client_start(const char *dashboard_url, bool rot180);

#ifdef __cplusplus
}
#endif
