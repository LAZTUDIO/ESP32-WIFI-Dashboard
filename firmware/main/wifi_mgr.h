/**
 * Wi-Fi 管理：STA 连接 + SoftAP 配网热点
 */
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_AP_SSID "ESP32-WIFI-Dashboard"
#define WIFI_AP_PASS "12345678"
#define WIFI_AP_IP   "192.168.4.1"

esp_err_t wifi_mgr_init(void);

/** 阻塞连接，成功返回 true（timeout_ms 内拿到 IP）；超时后仍会后台重连 */
bool wifi_mgr_sta_connect(const char *ssid, const char *pass, int timeout_ms);
void wifi_mgr_sta_disconnect(void);
/** 开关掉线自动重连，返回旧值 */
bool wifi_mgr_auto_reconnect(bool on);
bool wifi_mgr_sta_connected(void);
bool wifi_mgr_sta_ip(char *out, size_t n);
int8_t wifi_mgr_rssi(void);

/** 开启配网热点（APSTA，保留 STA 不断开） */
bool wifi_mgr_ap_start(void);
void wifi_mgr_ap_stop(void);
bool wifi_mgr_ap_running(void);

/* 扫描附近 AP，返回写入 out 的条数；每条为 "ssid|rssi" */
int wifi_mgr_scan(char *out, size_t cap, int max_items);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MGR_H */
