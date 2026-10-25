/* PC 模拟器用的 ESP-IDF 头文件替身：堆分配退化为 malloc/calloc/free */
#ifndef SIM_ESP_HEAP_CAPS_H
#define SIM_ESP_HEAP_CAPS_H

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_INTERNAL 0

static inline void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    return calloc(n, size);
}

static inline void *heap_caps_malloc(size_t size, int caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr) { free(ptr); }

#endif /* SIM_ESP_HEAP_CAPS_H */
