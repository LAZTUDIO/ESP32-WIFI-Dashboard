/**
 * 服务端布局驱动的动态看板 UI（M2）。
 * build 时解析布局 JSON、创建 LVGL 屏幕，并生成 WS 订阅消息；
 * apply 时从 dash_metric_store 读取最新值刷新组件。
 */
#ifndef UI_DYNAMIC_H
#define UI_DYNAMIC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 必须在 LVGL 锁内调用。subscribe_out 写入 WS 订阅 JSON。 */
bool ui_dynamic_build(const char *layout_json, char *subscribe_out,
                      size_t subscribe_cap);

/** 从指标存储刷新所有组件；必须在 LVGL 锁内调用。 */
void ui_dynamic_apply(void);

bool ui_dynamic_active(void);

/** 删除动态屏幕；必须在 LVGL 锁内调用。 */
void ui_dynamic_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_DYNAMIC_H */
