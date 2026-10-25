/**
 * 传统框架 —— 固定式界面主题（非小组件网格）。
 *
 * 与 ui_dynamic 的分工：ui_dynamic 解析布局 JSON、按 framework 字段分流；
 * framework == "classic" 时由本模块整屏渲染固定主题，指标订阅为固定集合。
 *
 * 现有主题（均为横屏 480×320 设计，画布不同时按比例缩放）：
 *   instrument  磷光示波器（第 2 组 I01 配色）
 *   hud         战术 HUD（第 6 组 H01 配色）
 *
 * 显示内容（两套主题一致）：模型名称、LLM/TTS/Embedding 加载状态、
 * GPU/CPU/NPU/内存/磁盘占用率、KV 池、并发数、排队数、GPU 温度、GPU 功率。
 * 文案全部 ASCII，只用内置 Montserrat，不依赖服务端字体子集。
 */
#ifndef UI_CLASSIC_H
#define UI_CLASSIC_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_classic ui_classic_t;

/** 传统主题固定订阅的指标 id 列表（用于生成 subscribe JSON）。 */
const char **ui_classic_metric_ids(int *count);

/** 按主题 id 构建整屏；失败返回 NULL。canvas 为当前屏幕物理尺寸。 */
ui_classic_t *ui_classic_build(const char *theme_id, int canvas_w,
                               int canvas_h);

/** 取构建出的屏幕对象（随 destroy 一起删除）。 */
lv_obj_t *ui_classic_screen(ui_classic_t *c);

/** 从指标存储刷新全部数值；需在 LVGL 锁内调用。 */
void ui_classic_apply(ui_classic_t *c);

/** 释放屏幕与定时器；幂等，可传 NULL。 */
void ui_classic_destroy(ui_classic_t *c);

#ifdef __cplusplus
}
#endif

#endif /* UI_CLASSIC_H */
