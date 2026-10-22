/**
 * 串口调试控制台
 */
#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/** 启动命令读取任务与性能统计任务 */
void debug_console_start(void);

/**
 * 注册主题/方向调试回调：theme=-1 保持不变；rot180=-1 保持不变，2 表示翻转。
 * 供 t / r 串口命令使用。
 */
typedef void (* dbg_view_cb_t)(int theme, int rot180);
void debug_console_set_view_cb(dbg_view_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_CONSOLE_H */
