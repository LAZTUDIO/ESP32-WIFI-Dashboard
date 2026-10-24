#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** STA 模式下启动设备管理 HTTP（:80）：info / config / reboot。 */
esp_err_t device_api_start(void);
void device_api_stop(void);
bool device_api_running(void);

#ifdef __cplusplus
}
#endif
