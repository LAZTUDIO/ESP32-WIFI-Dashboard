/**
 * 服务端布局驱动的动态看板 UI（M2）。
 * build 时解析布局 JSON、创建 LVGL 屏幕，并生成 WS 订阅消息；
 * apply 时从 dash_metric_store 读取最新值刷新组件。
 */
#ifndef UI_DYNAMIC_H
#define UI_DYNAMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设置动态 UI 使用的三档中文字体（服务端下发的 11/13/15 binfont）；
 *  任一参数为 NULL 时该档回退内置 16px 子集。需在 LVGL 锁内调用。 */
void ui_dynamic_set_cjk_fonts(const lv_font_t *small,
                              const lv_font_t *medium,
                              const lv_font_t *large);

typedef struct {
    uint32_t apply_count;
    int64_t apply_last_us;
    int64_t apply_avg_us;
    int64_t apply_max_us;
    int64_t page_last_ms;
    uint32_t page_swaps;
    int64_t trans_last_ms;
} ui_dynamic_perf_t;

/** 构建新布局到暂存区（不切换屏幕）。成功后需调用 ui_dynamic_commit()。
 *  必须在 LVGL 锁内调用。subscribe_out 写入 WS 订阅 JSON。 */
bool ui_dynamic_build(const char *layout_json, char *subscribe_out,
                      size_t subscribe_cap);

/** 把暂存布局切到前台并释放旧屏；必须在 LVGL 锁内调用。 */
void ui_dynamic_commit(void);

int ui_dynamic_widget_count(void);

/** 快速统计 JSON 中受支持的组件数（用于决定能否原子换屏）。 */
int ui_dynamic_json_widget_count(const char *layout_json);

/** 从指标存储刷新所有组件；必须在 LVGL 锁内调用。 */
void ui_dynamic_apply(void);

/** 刷新耗时/翻页耗时统计（用于串口性能观测）。 */
void ui_dynamic_perf(ui_dynamic_perf_t *out);

bool ui_dynamic_active(void);

/** 动态模式是否已经接管过屏幕（重建期间仍为 true）。 */
bool ui_dynamic_engaged(void);

/** 切到指定页；必须在 LVGL 锁内调用。 */
void ui_dynamic_set_page(int page);

int ui_dynamic_page_count(void);

/** 0=none, 1=slide, 2=fade, 3=stack, 4=zoom, 5=flip, 6=rotate */
int ui_dynamic_page_anim(void);

/** 临时切换当前布局的翻页动画（不落盘），用于调试对比。 */
void ui_dynamic_set_page_anim(int mode);

/** 显示/隐藏配网提示浮层；必须在 LVGL 锁内调用。 */
void ui_dynamic_show_portal(bool show, const char *ssid, const char *pass,
                            const char *ip);

/** 显示/隐藏网络信息浮层（STA 已连接、等待服务端）；必须在 LVGL 锁内调用。 */
void ui_dynamic_show_network(bool show, const char *ssid, const char *ip,
                             const char *device_id, const char *name,
                             bool server_connected);

/** 删除动态屏幕；必须在 LVGL 锁内调用。 */
void ui_dynamic_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_DYNAMIC_H */
