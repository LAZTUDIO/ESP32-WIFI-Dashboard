/**
 * 小组件 2.0 —— 服务端布局驱动的第二代表面库。
 *
 * 与 ui_dynamic.c 的分工：ui_dynamic 负责卡片外壳、标题栏、主题 token 与
 * 指标订阅；ui_mini 负责 10 种新组件类型的内部渲染与逐秒刷新。
 *
 * 约束（与 ui.h 一致）：仅用 LVGL 图元（标签/圆弧/条/图表/圆角矩形/绘制
 * 事件补线），不用滤镜、毛玻璃、3D 变换；动画只做透明度/数值/旋转。
 */
#ifndef UI_MINI_H
#define UI_MINI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "dash_metric_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MINI_MAX_BINDS 8
#define UI_MINI_MAX_ROWS 16
#define UI_MINI_HIST 60
#define UI_MINI_ROW_HIST 24

/* 组件目录里 generation=2 的类型集合 */
bool ui_mini_is_type(const char *type);
const char *ui_mini_label(const char *type);

/* ui_dynamic 解析后的绑定快照（指标指针已解析） */
typedef struct {
    char key[16];
    dash_metric_t *metric;
    char label[48];
    char unit[16];
    int precision;
    char mtype[12];
} ui_mini_bind_t;

typedef struct {
    char label[32];
    dash_metric_t *metric;
    char kind[12];
    char unit[16];
    int precision;
    char mtype[12];
} ui_mini_row_t;

typedef struct {
    char type[24];
    char title[40];
    char unit[16];
    char unit2[16];
    char on_text[16];
    char off_text[16];
    int font_size; /* 0=S 1=M 2=L */
    int align;     /* 0=left 1=center */
    bool show_ticks;
    bool show_labels;
    bool show_values;
    bool show_trend;
    bool show_area; /* dual_line 面积填充 */
    double max_value;
    double warn_above;
    double danger_above;
} ui_mini_spec_t;

typedef struct ui_mini ui_mini_t;

/** 在卡片正文容器里创建组件；parent 需已定位定尺寸。
 *  body_w/body_h 由调用方（ui_dynamic）显式给出：LVGL 9 的 coords 要等
 *  布局刷新才更新，创建时查询不可靠。 */
ui_mini_t *ui_mini_create(lv_obj_t *parent, int body_w, int body_h,
                          const ui_mini_spec_t *spec, int gw, int gh,
                          const ui_mini_bind_t *binds, int n_binds,
                          const ui_mini_row_t *rows, int n_rows);

/** 每秒刷新：读指标存储更新所有子对象。 */
void ui_mini_apply(ui_mini_t *m);

/** 释放结构体本身（LVGL 子对象随卡片删除，不在这里碰）。 */
void ui_mini_destroy(ui_mini_t *m);

/** 共享动画时钟：LED 呼吸/离线闪烁，返回 0..999 的相位。 */
uint32_t ui_mini_phase(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MINI_H */
