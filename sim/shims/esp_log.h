/* PC 模拟器用的 ESP-IDF 日志替身 */
#ifndef SIM_ESP_LOG_H
#define SIM_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGE(tag, ...)            \
    do {                              \
        fprintf(stderr, "E %s: ", tag); \
        fprintf(stderr, __VA_ARGS__);   \
        fprintf(stderr, "\n");          \
    } while (0)
#define ESP_LOGW(tag, ...)         \
    do {                           \
        printf("W %s: ", tag);     \
        printf(__VA_ARGS__);       \
        printf("\n");              \
    } while (0)
#define ESP_LOGI(tag, ...)         \
    do {                           \
        printf("I %s: ", tag);     \
        printf(__VA_ARGS__);       \
        printf("\n");              \
    } while (0)
#define ESP_LOGD(tag, ...) (void)0
#define ESP_LOGV(tag, ...) (void)0

#endif /* SIM_ESP_LOG_H */
