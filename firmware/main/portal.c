#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "portal.h"
#include "wifi_mgr.h"

static const char *TAG = "portal";

static httpd_handle_t s_server;
static bool s_running;
static volatile bool s_dns_stop;
static int s_dns_sock = -1;

/* ============================ 工具 ============================ */

static void html_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; p && *p && o + 1 < n; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (o + l >= n) break;
            memcpy(out + o, rep, l);
            o += l;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t n)
{
    size_t o = 0;
    while (*src && o + 1 < n) {
        if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            dst[o++] = (char)((hexval(src[1]) << 4) | hexval(src[2]));
            src += 3;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
}

/** 从 application/x-www-form-urlencoded 里取字段 */
static bool form_value(const char *body, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *amp = strchr(eq, '&');
        size_t name_len = (size_t)(eq - p);
        if (name_len == kl && strncmp(p, key, kl) == 0) {
            size_t val_len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
            char tmp[512];
            if (val_len >= sizeof(tmp)) val_len = sizeof(tmp) - 1;
            memcpy(tmp, eq + 1, val_len);
            tmp[val_len] = '\0';
            url_decode(tmp, out, n);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* ============================ 页面 ============================ */

static void append(char **buf, size_t *used, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < cap - *used) *used += (size_t)n;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    const size_t cap = 6144;
    char *html = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) return httpd_resp_send_500(req);

    app_config_t cfg;
    config_load(&cfg);

    char scan[900];
    int found = wifi_mgr_scan(scan, sizeof(scan), 16);

    char ssid_esc[160], pass_esc[160], server_esc[220], key_esc[260], host_esc[220];
    char dashboard_esc[220];
    html_escape(cfg.wifi_ssid, ssid_esc, sizeof(ssid_esc));
    html_escape(cfg.wifi_pass, pass_esc, sizeof(pass_esc));
    html_escape(cfg.server_url, server_esc, sizeof(server_esc));
    html_escape(cfg.api_key, key_esc, sizeof(key_esc));
    html_escape(cfg.host_agent, host_esc, sizeof(host_esc));
    html_escape(cfg.dashboard_url, dashboard_esc, sizeof(dashboard_esc));

    size_t used = 0;
    html[0] = '\0';
    append(&html, &used, cap,
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>LLM 看板配网</title><style>"
        "*{box-sizing:border-box}"
        "body{margin:0;background:#0b0f16;color:#e8eef7;font:15px/1.5 -apple-system,"
        "\"PingFang SC\",\"Microsoft YaHei\",sans-serif}"
        ".w{max-width:420px;margin:0 auto;padding:24px 18px 40px}"
        "h1{font-size:22px;margin:0 0 4px}"
        ".s{color:#7b8ca3;font-size:13px;margin:0 0 22px}"
        "label{display:block;color:#8c9bae;font-size:12px;margin:16px 0 6px}"
        "input,select{width:100%%;padding:11px 12px;border-radius:10px;"
        "border:1px solid #263242;background:#121822;color:#e8eef7;font-size:15px;"
        "-webkit-appearance:none}"
        "input:focus,select:focus{outline:none;border-color:#4d8dff}"
        "button{width:100%%;margin-top:24px;padding:13px;border:0;border-radius:12px;"
        "background:linear-gradient(135deg,#4d8dff,#8b5cf6);color:#fff;"
        "font-size:16px;font-weight:600}"
        ".f{color:#5a6b80;font-size:12px;margin-top:14px;text-align:center}"
        ".badge{display:inline-block;padding:3px 9px;border-radius:99px;"
        "background:#12243d;color:#4d8dff;font-size:11px;margin-bottom:10px}"
        ".sec{margin:22px 0 6px;padding-top:14px;border-top:1px solid #263242;"
        "color:#8c9bae;font-size:12px;font-weight:600;letter-spacing:.4px}"
        "</style></head><body><div class=\"w\">"
        "<div class=\"badge\">ESP32-S3 · LLM 监控看板</div>"
        "<h1>设备配网</h1><p class=\"s\">连接 Wi-Fi，并按需填写两路数据源（可只填一个）</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<label>扫描到的 Wi-Fi（%d 个）</label><select name=\"sel\">"
        "<option value=\"\">— 手动输入下方的名称 —</option>", found);

    char *save = scan;
    while (*save && used < cap - 200) {
        char *nl = strchr(save, '\n');
        if (nl) *nl = '\0';
        char *bar = strchr(save, '|');
        if (bar) {
            *bar = '\0';
            char esc[96];
            html_escape(save, esc, sizeof(esc));
            if (strcmp(save, cfg.wifi_ssid) == 0) {
                append(&html, &used, cap,
                       "<option value=\"%s\" selected>%s (%s dBm)</option>",
                       esc, esc, bar + 1);
            } else {
                append(&html, &used, cap,
                       "<option value=\"%s\">%s (%s dBm)</option>",
                       esc, esc, bar + 1);
            }
        }
        if (!nl) break;
        save = nl + 1;
    }

    append(&html, &used, cap,
        "</select>"
        "<label>Wi-Fi 名称</label>"
        "<input name=\"ssid\" placeholder=\"手动输入优先\" value=\"%s\">"
        "<label>Wi-Fi 密码</label>"
        "<input name=\"pass\" type=\"password\" value=\"%s\">"
        "<div class=\"sec\">① 传统 API（设备直连 LLM 服务）</div>"
        "<label>API 地址（留空则不启用直连）</label>"
        "<input name=\"server\" placeholder=\"http://192.168.1.100:8000\" value=\"%s\">"
        "<label>API 密钥</label>"
        "<input name=\"key\" type=\"password\" value=\"%s\">"
        "<label>服务类型</label><select name=\"provider\">",
        ssid_esc, pass_esc, server_esc, key_esc);

    static const char *provs[][2] = {
        {"auto", "自动识别"}, {"panel", "Halogen 面板 (8800)"},
        {"halogen", "Halogen"}, {"gufo", "Gufo"},
        {"strata", "Strata"}, {"lmstudio", "LM Studio"},
        {"ollama", "Ollama"}, {"openai", "OpenAI 兼容"},
    };
    for (size_t i = 0; i < sizeof(provs) / sizeof(provs[0]); i++) {
        append(&html, &used, cap, "<option value=\"%s\"%s>%s</option>",
               provs[i][0],
               strcmp(cfg.provider, provs[i][0]) == 0 ? " selected" : "",
               provs[i][1]);
    }

    append(&html, &used, cap,
        "</select>"
        "<label>主机监控地址（可选，也可填 8800 面板地址）</label>"
        "<input name=\"host\" placeholder=\"http://192.168.1.100:8800\" value=\"%s\">"
        "<div class=\"sec\">② 专用看板 Server（布局 / 配色 / 服务端指标）</div>"
        "<label>Server 地址（留空则不启用动态看板）</label>"
        "<input name=\"dashboard\" placeholder=\"http://192.168.1.100:8080\" value=\"%s\">"
        "<button type=\"submit\">保存并重启</button>"
        "</form><p class=\"f\">保存后设备会自动重启并连接</p>"
        "</div></body></html>", host_esc, dashboard_esc);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    heap_caps_free(html);
    return err;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[1536];
    int total = 0;
    int len = req->content_len;
    if (len >= (int)sizeof(body)) return httpd_resp_send_err(req, 400, "too large");
    while (total < len) {
        int r = httpd_req_recv(req, body + total, len - total);
        if (r <= 0) return httpd_resp_send_err(req, 400, "recv failed");
        total += r;
    }
    body[total] = '\0';

    app_config_t cfg;
    config_load(&cfg);

    char v[192];
    if (form_value(body, "sel", v, sizeof(v)) && v[0]) {
        snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%.*s",
                 (int)sizeof(cfg.wifi_ssid) - 1, v);
    }
    if (form_value(body, "ssid", v, sizeof(v)) && v[0]) {
        snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%.*s",
                 (int)sizeof(cfg.wifi_ssid) - 1, v);
    }
    if (form_value(body, "pass", v, sizeof(v))) {
        snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%.*s",
                 (int)sizeof(cfg.wifi_pass) - 1, v);
    }
    if (form_value(body, "server", v, sizeof(v))) {
        snprintf(cfg.server_url, sizeof(cfg.server_url), "%.*s",
                 (int)sizeof(cfg.server_url) - 1, v);
    }
    if (form_value(body, "key", v, sizeof(v))) {
        snprintf(cfg.api_key, sizeof(cfg.api_key), "%.*s",
                 (int)sizeof(cfg.api_key) - 1, v);
    }
    if (form_value(body, "provider", v, sizeof(v)) && v[0]) {
        snprintf(cfg.provider, sizeof(cfg.provider), "%.*s",
                 (int)sizeof(cfg.provider) - 1, v);
    }
    if (form_value(body, "host", v, sizeof(v))) {
        snprintf(cfg.host_agent, sizeof(cfg.host_agent), "%.*s",
                 (int)sizeof(cfg.host_agent) - 1, v);
    }
    if (form_value(body, "dashboard", v, sizeof(v))) {
        snprintf(cfg.dashboard_url, sizeof(cfg.dashboard_url), "%.*s",
                 (int)sizeof(cfg.dashboard_url) - 1, v);
    }
    config_normalize(&cfg);
    config_save(&cfg);

    ESP_LOGI(TAG, "provisioned: ssid=%s api=%s dashboard=%s",
             cfg.wifi_ssid,
             cfg.server_url[0] ? cfg.server_url : "(none)",
             cfg.dashboard_url[0] ? cfg.dashboard_url : "(none)");

    const char *page =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{background:#0b0f16;color:#e8eef7;font:16px/1.6 -apple-system,"
        "\"PingFang SC\",sans-serif;text-align:center;padding:80px 20px}"
        "h2{color:#34d399}</style></head><body>"
        "<h2>配置已保存</h2><p>设备正在重启，请稍候…</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, page);

    const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1500 * 1000);
    }
    return ESP_OK;
}

/** Captive Portal：任何未知路径都跳回配置页 */
static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_AP_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ============================ DNS 劫持 ============================ */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = inet_addr(WIFI_AP_IP);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS spoof listening on " WIFI_AP_IP ":53");
    s_dns_sock = sock;

    uint8_t buf[512];
    uint8_t resp[512];
    while (!s_dns_stop) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || n > (int)sizeof(buf)) continue;

        memcpy(resp, buf, 12);
        resp[2] = 0x81;             /* QR=1, opcode=0, RD=1 */
        resp[3] = 0x80;             /* RA=1, rcode=0 */
        resp[6] = 0x00;             /* ANCOUNT = 1 */
        resp[7] = 0x01;
        resp[8] = resp[9] = resp[10] = resp[11] = 0x00;

        int qend = 12;
        while (qend < n && buf[qend] != 0) qend += buf[qend] + 1;
        qend += 5;
        if (qend > n) continue;
        memcpy(resp + 12, buf + 12, (size_t)(qend - 12));

        int p = qend;
        resp[p++] = 0xC0; resp[p++] = 0x0C;         /* name -> question */
        resp[p++] = 0x00; resp[p++] = 0x01;         /* type A */
        resp[p++] = 0x00; resp[p++] = 0x01;         /* class IN */
        resp[p++] = 0x00; resp[p++] = 0x00;
        resp[p++] = 0x00; resp[p++] = 0x1E;         /* ttl 30 */
        resp[p++] = 0x00; resp[p++] = 0x04;
        resp[p++] = 192; resp[p++] = 168; resp[p++] = 4; resp[p++] = 1;

        sendto(sock, resp, (size_t)p, 0, (struct sockaddr *)&from, fl);
    }

    close(sock);
    s_dns_sock = -1;
    vTaskDelete(NULL);
}

/* ============================ 启动 ============================ */

esp_err_t portal_start(const app_config_t *cfg)
{
    (void)cfg;
    if (s_running) return ESP_OK;

    if (!wifi_mgr_ap_start()) return ESP_FAIL;

    s_dns_stop = false;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.stack_size = 8192;
    hc.lru_purge_enable = true;
    hc.server_port = 80;

    if (httpd_start(&s_server, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t any  = { .uri = "/*", .method = HTTP_GET, .handler = handle_redirect };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &any);

    xTaskCreate(dns_task, "dns_spoof", 4096, NULL, 4, NULL);

    s_running = true;
    ESP_LOGI(TAG, "portal ready at http://" WIFI_AP_IP);
    return ESP_OK;
}

bool portal_running(void)
{
    return s_running;
}

void portal_stop(void)
{
    if (!s_running) return;

    s_running = false;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_dns_stop = true;
    if (s_dns_sock >= 0) shutdown(s_dns_sock, SHUT_RDWR);
    wifi_mgr_ap_stop();
    ESP_LOGI(TAG, "portal stopped");
}
