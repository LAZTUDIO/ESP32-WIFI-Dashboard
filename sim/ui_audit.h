#ifndef UI_AUDIT_H
#define UI_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 渲染后自动巡检：越界 / 同级重叠 / 文字溢出 / 低对比度。
 * 结果直接打印到 stdout，供无头渲染闭环判断。
 */
void ui_audit_run(int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* UI_AUDIT_H */
