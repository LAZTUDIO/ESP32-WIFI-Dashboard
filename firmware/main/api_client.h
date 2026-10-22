/**
 * 极简 HTTP GET 客户端（ESP-IDF esp_http_client 包装）
 */
#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GET url（可带 Bearer），把响应正文写进 out（以 '\0' 结尾）。
 * @return 正文长度(>=0)，-1 传输失败，-2 HTTP 非 2xx
 */
int api_get(const char *url, const char *bearer, char *out, size_t cap,
            int timeout_ms);

/** 拼接 base + path 后再 GET */
int api_get_path(const char *base, const char *path, const char *bearer,
                 char *out, size_t cap, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* API_CLIENT_H */
