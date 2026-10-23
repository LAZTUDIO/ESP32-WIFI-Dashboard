/**
 * 动态看板的指标值存储：服务端快照写入，组件渲染读取。
 * 固定容量，PSRAM 分配，避免逐帧 malloc。
 */
#ifndef DASH_METRIC_STORE_H
#define DASH_METRIC_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_METRIC_MAX 96
#define DASH_METRIC_ID_LEN 40
#define DASH_METRIC_TEXT_LEN 96

typedef enum {
    DASH_METRIC_NUMBER = 0,
    DASH_METRIC_BOOL,
    DASH_METRIC_STRING,
} dash_metric_type_t;

typedef struct {
    char id[DASH_METRIC_ID_LEN];
    uint8_t type;
    bool available;
    double number;
    bool boolean;
    char text[DASH_METRIC_TEXT_LEN];
} dash_metric_t;

bool dash_metric_store_init(void);
dash_metric_t *dash_metric_store_find(const char *id);
dash_metric_t *dash_metric_store_get(const char *id);
void dash_metric_store_set_number(const char *id, double value, bool available);
void dash_metric_store_set_bool(const char *id, bool value, bool available);
void dash_metric_store_set_string(const char *id, const char *value, bool available);
void dash_metric_store_mark_unavailable(const char *id);
void dash_metric_store_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* DASH_METRIC_STORE_H */
