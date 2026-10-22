/**
 * 主题模块内部接口 —— 只供 ui_dashboard.c 与各主题实现使用。
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 横屏 480×320，ROG 红黑斜切；返回独立 screen */
lv_obj_t * ui_strix_create(void);
void ui_strix_apply(const dash_metrics_t * m, const dash_history_t * h,
                    bool new_sample);
void ui_strix_set_page(int page);
int  ui_strix_page_count(void);

/* 竖屏 320×480，TUF 石墨琥珀；返回独立 screen */
lv_obj_t * ui_tuf_create(void);
void ui_tuf_apply(const dash_metrics_t * m, const dash_history_t * h,
                  bool new_sample);
void ui_tuf_set_page(int page);
int  ui_tuf_page_count(void);

/** 主题里的按钮把动作抛回 dashboard，由上层决定持久化/重启 */
void dashboard_ui_emit(dash_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
