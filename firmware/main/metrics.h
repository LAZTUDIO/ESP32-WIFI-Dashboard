/**
 * 指标采集：自动识别 provider 并轮询出统一的 dash_metrics_t
 *
 * 支持 halogen / gufo（llama.cpp 系）、strata、LM Studio、Ollama、OpenAI 兼容。
 * halogen/gufo 的 TPS 用 /metrics 计数器做区间差分；
 * strata 直接读 /api/monitor；
 * 功耗/温度/uptime 可由可选的主机监控 agent 补齐。
 */
#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>

#include "app_config.h"
#include "dash_model.h"

#ifdef __cplusplus
extern "C" {
#endif

void metrics_init(void);

/** 采集一次；失败时会把上一次数据标记为 stale */
bool metrics_poll(const app_config_t *cfg, dash_metrics_t *out);

/** 把一次采集结果写进动态 UI 的指标存储（与 WS 数据共用同一份 store）。 */
void metrics_publish(const dash_metrics_t *m);

void metrics_reset_peak(void);
dash_provider_t metrics_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* METRICS_H */
