#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动动态看板客户端：等待 Wi-Fi → 拉取该设备的布局 → 建立 WS 订阅。
 * dashboard_url 形如 http://10.0.0.2:8080；为空时任务待命，等配置下发后再连。
 */
void dashboard_client_start(const char *dashboard_url, const char *device_id,
                            bool rot180);

/** 运行时切换服务端/设备 ID/旋转（配置热更新），会重连并重拉布局。 */
void dashboard_client_reconfigure(const char *dashboard_url,
                                  const char *device_id, bool rot180);

#ifdef __cplusplus
}
#endif
