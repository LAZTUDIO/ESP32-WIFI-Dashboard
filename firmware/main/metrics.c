#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "cJSON.h"

#include "metrics.h"
#include "api_client.h"
#include "dash_metric_store.h"
#include "wifi_mgr.h"

static const char *TAG = "metrics";

#define BUF_BIG  24576
#define BUF_SMALL 8192

static char *s_buf;
static char *s_small;

static dash_provider_t s_prov;
static bool s_probed;
static char s_base[160];
static char s_key[192];
static char s_provider[16];

static int s_fail;
static bool s_have_last;
static dash_metrics_t s_last;

static float s_peak_dec;
static float s_peak_pre;

/* halogen / gufo 计数器差分 */
static uint64_t s_prev_pred, s_prev_pred_sec, s_prev_prompt, s_prev_prompt_sec;
static int64_t s_prev_us;
static bool s_diff_primed;
static float s_last_dec, s_last_pre;

/* gufo slots 差分 */
static uint64_t s_gufo_decoded;
static int64_t s_gufo_us;

/* ============================ JSON 工具 ============================ */

static cJSON *jpath(cJSON *root, const char *path)
{
    if (!root) return NULL;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", path);

    cJSON *cur = root;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
        cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
        if (!cur) return NULL;
    }
    return cur;
}

static bool jnum_try(cJSON *root, const char *path, double *out)
{
    cJSON *v = jpath(root, path);
    if (!v) return false;
    if (cJSON_IsNumber(v)) { *out = v->valuedouble; return true; }
    if (cJSON_IsString(v)) { *out = atof(v->valuestring); return true; }
    if (cJSON_IsBool(v)) { *out = cJSON_IsTrue(v) ? 1 : 0; return true; }
    return false;
}

static double jnum(cJSON *root, const char *path, double def)
{
    double v;
    return jnum_try(root, path, &v) ? v : def;
}

static bool jstr(cJSON *root, const char *path, char *out, size_t n)
{
    cJSON *v = jpath(root, path);
    if (!v || !cJSON_IsString(v)) return false;
    snprintf(out, n, "%s", v->valuestring);
    return true;
}

static bool jbool(cJSON *root, const char *path)
{
    cJSON *v = jpath(root, path);
    return v && cJSON_IsTrue(v);
}

/* ============================ 文本指标 ============================ */

/** 在 Prometheus 文本里找 "key value"，key 必须在行首 */
static bool mval(const char *buf, const char *key, double *out)
{
    size_t kl = strlen(key);
    const char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        bool line_start = (p == buf) || (p[-1] == '\n');
        if (line_start && (p[kl] == ' ' || p[kl] == '\t')) {
            *out = atof(p + kl + 1);
            return true;
        }
        p += kl;
    }
    return false;
}

/* ============================ 识别 ============================ */

static dash_provider_t provider_from_name(const char *name)
{
    if (!name) return DASH_PROV_UNKNOWN;
    if (strcmp(name, "halogen") == 0) return DASH_PROV_HALOGEN;
    if (strcmp(name, "gufo") == 0) return DASH_PROV_GUFO;
    if (strcmp(name, "strata") == 0) return DASH_PROV_STRATA;
    if (strcmp(name, "lmstudio") == 0) return DASH_PROV_LMSTUDIO;
    if (strcmp(name, "ollama") == 0) return DASH_PROV_OLLAMA;
    if (strcmp(name, "openai") == 0) return DASH_PROV_OPENAI;
    if (strcmp(name, "panel") == 0) return DASH_PROV_PANEL;
    return DASH_PROV_UNKNOWN;
}

static const char *provider_name(dash_provider_t p)
{
    switch (p) {
    case DASH_PROV_HALOGEN: return "halogen";
    case DASH_PROV_GUFO:    return "gufo";
    case DASH_PROV_STRATA:  return "strata";
    case DASH_PROV_LMSTUDIO:return "LM Studio";
    case DASH_PROV_OLLAMA:  return "Ollama";
    case DASH_PROV_OPENAI:  return "OpenAI";
    case DASH_PROV_PANEL:   return "面板 8800";
    default:                return "未知";
    }
}

static bool probe(const app_config_t *cfg)
{
    const char *base = cfg->server_url;
    const char *key = cfg->api_key;

    int n;

    /* halogen-panel（8800）：/api/status 是面板聚合接口，无密钥即可读 */
    n = api_get_path(base, "/api/status", key, s_buf, BUF_BIG, 2500);
    if (n > 0 && strstr(s_buf, "\"container_uptime\"") &&
        strstr(s_buf, "\"metrics\"") && strstr(s_buf, "\"gpu\"") &&
        strstr(s_buf, "\"mem\"")) {
        s_prov = DASH_PROV_PANEL;
        ESP_LOGI(TAG, "detected halogen-panel");
        return true;
    }

    /* Strata 的 /api/monitor 最有辨识度 */
    n = api_get_path(base, "/api/monitor", key, s_buf, BUF_BIG, 2500);
    if (n > 0 && (strstr(s_buf, "outputTps") || strstr(s_buf, "gpuTempC"))) {
        s_prov = DASH_PROV_STRATA;
        ESP_LOGI(TAG, "detected strata");
        return true;
    }

    /* llama.cpp 系：/health 里有 model + slots */
    n = api_get_path(base, "/health", key, s_buf, BUF_BIG, 2500);
    if (n > 0 && strstr(s_buf, "\"model\"") && strstr(s_buf, "\"slots\"")) {
        s_prov = DASH_PROV_HALOGEN;
        int m = api_get_path(base, "/v1/slots", key, s_small, BUF_SMALL, 2000);
        if (m > 0 && s_small[0] == '[') s_prov = DASH_PROV_GUFO;
        ESP_LOGI(TAG, "detected %s", provider_name(s_prov));
        return true;
    }

    n = api_get_path(base, "/api/ps", key, s_buf, BUF_BIG, 2000);
    if (n > 0 && strstr(s_buf, "\"models\"")) {
        s_prov = DASH_PROV_OLLAMA;
        ESP_LOGI(TAG, "detected ollama");
        return true;
    }

    n = api_get_path(base, "/api/v0/models", key, s_buf, BUF_BIG, 2000);
    if (n > 0 && strstr(s_buf, "\"data\"")) {
        s_prov = DASH_PROV_LMSTUDIO;
        ESP_LOGI(TAG, "detected LM Studio");
        return true;
    }

    n = api_get_path(base, "/v1/models", key, s_small, BUF_SMALL, 2000);
    if (n > 0 && strstr(s_small, "\"data\"")) {
        s_prov = DASH_PROV_OPENAI;
        ESP_LOGI(TAG, "detected OpenAI-compatible");
        return true;
    }

    ESP_LOGW(TAG, "provider probe failed");
    return false;
}

/* ============================ halogen / gufo ============================ */

static void note_peak(float dec, float pre)
{
    if (dec > s_peak_dec) s_peak_dec = dec;
    if (pre > s_peak_pre) s_peak_pre = pre;
}

static bool parse_llamacpp_health(const char *json, dash_metrics_t *m)
{
    cJSON *r = cJSON_Parse(json);
    if (!r) return false;

    jstr(r, "model", m->model, sizeof(m->model));
    if (!jstr(r, "version.api", m->version, sizeof(m->version))) {
        jstr(r, "version", m->version, sizeof(m->version));
    }

    m->slots = (uint16_t)jnum(r, "slots", 0);
    m->has_slots = m->slots > 0;
    m->ctx_len = (uint32_t)jnum(r, "context", jnum(r, "slot_ctx", 0));

    double kv_pool = jnum(r, "kv_pool_positions", 0);
    if (kv_pool > 0) m->kv_total = (uint32_t)kv_pool;

    m->in_flight = (uint16_t)jnum(r, "in_flight", 0);
    m->has_concurrency = jpath(r, "in_flight") != NULL;
    m->queued = (uint16_t)jnum(r, "queued", 0);
    m->has_queue = jpath(r, "queued") != NULL;
    m->busy = jbool(r, "busy") || m->in_flight > 0;

    m->online = true;
    cJSON_Delete(r);
    return true;
}

static bool poll_llamacpp(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/health", cfg->api_key, s_buf, BUF_BIG, 2500);
    if (n <= 0 || !parse_llamacpp_health(s_buf, m)) return false;

    /* /metrics 在 halogen 上很完整；gufo 上可能没有，失败不影响在线状态 */
    n = api_get_path(cfg->server_url, "/metrics", cfg->api_key, s_buf, BUF_BIG, 3000);
    if (n > 0) {
        double v;

        uint64_t pred = 0, pred_sec = 0, prompt = 0, prompt_sec = 0;
        if (mval(s_buf, "llamacpp:tokens_predicted_total", &v)) pred = (uint64_t)v;
        if (mval(s_buf, "llamacpp:tokens_predicted_seconds_total", &v)) pred_sec = (uint64_t)(v * 1000.0);
        if (mval(s_buf, "llamacpp:prompt_tokens_total", &v)) prompt = (uint64_t)v;
        if (mval(s_buf, "llamacpp:prompt_seconds_total", &v)) prompt_sec = (uint64_t)(v * 1000.0);

        int64_t now = esp_timer_get_time();
        if (s_diff_primed && now > s_prev_us) {
            double dt = (double)(now - s_prev_us) / 1e6;
            double d_pred = (double)(pred - s_prev_pred);
            double d_sec = (double)(pred_sec - s_prev_pred_sec) / 1000.0;
            double d_prompt = (double)(prompt - s_prev_prompt);
            double d_prompt_sec = (double)(prompt_sec - s_prev_prompt_sec) / 1000.0;

            if (d_sec > 0.05 && d_pred > 0) {
                s_last_dec = (float)(d_pred / d_sec);
                m->tps_live = true;
            } else if (d_pred > 0 && dt > 0.2) {
                s_last_dec = (float)(d_pred / dt);
                m->tps_live = true;
            } else {
                m->tps_live = false;
            }
            if (d_prompt_sec > 0.05 && d_prompt > 0) {
                s_last_pre = (float)(d_prompt / d_prompt_sec);
            }
            note_peak(s_last_dec, s_last_pre);
        } else {
            m->tps_live = false;
        }

        s_prev_pred = pred;
        s_prev_pred_sec = pred_sec;
        s_prev_prompt = prompt;
        s_prev_prompt_sec = prompt_sec;
        s_prev_us = now;
        s_diff_primed = true;

        m->tps_dec = s_last_dec;
        m->tps_pre = s_last_pre;
        m->has_tps = true;

        if (pred_sec > 0 && pred > 0) {
            m->tps_avg = (float)((double)pred / ((double)pred_sec / 1000.0));
        }
        if (prompt_sec > 0 && prompt > 0) {
            m->tps_avg_pre = (float)((double)prompt / ((double)prompt_sec / 1000.0));
        }

        m->tps_peak = s_peak_dec;
        m->tps_peak_pre = s_peak_pre;

        if (mval(s_buf, "llamacpp:requests_processing", &v)) {
            m->in_flight = (uint16_t)v;
            m->has_concurrency = true;
        }
        if (mval(s_buf, "llamacpp:requests_deferred", &v)) {
            m->queued = (uint16_t)v;
            m->has_queue = true;
        }
        if (mval(s_buf, "llamacpp:kv_cache_usage_ratio", &v)) {
            m->kv_pct = (float)(v * 100.0);
            m->has_kv = true;
        } else if (mval(s_buf, "halogen:kv_pool_positions", &v) && m->kv_total > 0) {
            m->has_kv = true;
        }
        double used, total;
        if (mval(s_buf, "llamacpp:kv_cache_tokens", &used)) {
            m->kv_used = (uint32_t)used;
            if (m->kv_total > 0) {
                m->kv_pct = (float)(used * 100.0 / m->kv_total);
            }
            m->has_kv = true;
        } else if (mval(s_buf, "halogen:kv_pool_positions", &total)) {
            m->kv_total = (uint32_t)total;
        }

        if (mval(s_buf, "llamacpp:prompt_tokens_total", &v)) {
            m->tokens_in = (uint64_t)v;
            m->has_usage = true;
        }
        if (mval(s_buf, "llamacpp:tokens_predicted_total", &v)) {
            m->tokens_out = (uint64_t)v;
            m->has_usage = true;
        }
        if (mval(s_buf, "halogen:prompt_tokens_cached_total", &v)) {
            m->tokens_cached = (uint64_t)v;
        }
        if (mval(s_buf, "halogen:requests_total", &v) && v > 0) {
            m->requests_total = (uint64_t)v;
            m->has_requests = true;
        } else if (mval(s_buf, "llamacpp:requests_total", &v)) {
            m->requests_total = (uint64_t)v;
            m->has_requests = true;
        }

        double hits, misses, bytes, entries;
        if (mval(s_buf, "halogen:cache_hits_total", &hits) &&
            mval(s_buf, "halogen:cache_misses_total", &misses)) {
            double tot = hits + misses;
            if (tot > 0) {
                m->cache_hit_pct = (float)(hits * 100.0 / tot);
                m->has_cache = true;
            }
        }
        if (mval(s_buf, "halogen:cache_bytes", &bytes)) {
            m->cache_bytes = (uint64_t)bytes;
            m->has_cache = true;
        }
        if (mval(s_buf, "halogen:cache_entries", &entries)) {
            m->cache_entries = (uint32_t)entries;
        }

        double draft, accepted;
        if (mval(s_buf, "halogen:draft_tokens_total", &draft) && draft > 0 &&
            mval(s_buf, "halogen:draft_tokens_accepted_total", &accepted)) {
            m->mtp_pct = (float)(accepted * 100.0 / draft);
            m->has_mtp = true;
        }
    }

    /* gufo 没有 /metrics 时，用 /v1/slots 的 next_token.n_decoded 求和算 TPS */
    if (s_prov == DASH_PROV_GUFO && !m->has_tps) {
        int sn = api_get_path(cfg->server_url, "/v1/slots", cfg->api_key,
                              s_small, BUF_SMALL, 2000);
        if (sn > 0 && s_small[0] == '[') {
            cJSON *arr = cJSON_Parse(s_small);
            if (arr && cJSON_IsArray(arr)) {
                uint64_t sum = 0;
                cJSON *slot;
                cJSON_ArrayForEach(slot, arr) {
                    sum += (uint64_t)jnum(slot, "next_token.n_decoded", 0);
                }
                int64_t now = esp_timer_get_time();
                if (s_gufo_us > 0 && now > s_gufo_us && sum > s_gufo_decoded) {
                    double dt = (double)(now - s_gufo_us) / 1e6;
                    s_last_dec = (float)((double)(sum - s_gufo_decoded) / dt);
                    m->tps_live = true;
                    note_peak(s_last_dec, s_last_pre);
                } else {
                    m->tps_live = false;
                }
                s_gufo_decoded = sum;
                s_gufo_us = now;

                m->tps_dec = s_last_dec;
                m->tps_peak = s_peak_dec;
                m->has_tps = true;

                cJSON *first = cJSON_GetArrayItem(arr, 0);
                if (first) {
                    if (m->model[0] == '\0') jstr(first, "model", m->model, sizeof(m->model));
                    double nctx = jnum(first, "n_ctx", 0);
                    if (nctx > 0) m->ctx_len = (uint32_t)nctx;
                    if (m->slots == 0) {
                        m->slots = (uint16_t)cJSON_GetArraySize(arr);
                        m->has_slots = true;
                    }
                }
            }
            cJSON_Delete(arr);
        }
    }

    return true;
}

/* ============================ halogen-panel (8800 /api/status) ============================ */

/** 面板返回中文时长（"7天2小时41分"），换算成秒 */
static uint32_t parse_cn_duration(const char *s)
{
    uint32_t total = 0;
    const char *p = s ? s : "";
    while (*p) {
        while (*p && (*p < '0' || *p > '9')) p++;
        if (!*p) break;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
        if (strncmp(p, "天", 3) == 0)        { total += v * 86400U; p += 3; }
        else if (strncmp(p, "小时", 6) == 0) { total += v * 3600U;  p += 6; }
        else if (strncmp(p, "分", 3) == 0)   { total += v * 60U;    p += 3; }
        else if (strncmp(p, "秒", 3) == 0)   { total += v;          p += 3; }
    }
    return total;
}

/** 面板硬件段（gpu/mem/uptime）→ 统一指标；host_agent 复用 */
static void parse_panel_hw(cJSON *r, dash_metrics_t *m)
{
    double v;
    char buf[64];

    cJSON *gpu = jpath(r, "gpu");
    if (gpu) {
        if (jnum_try(gpu, "power_w", &v)) {
            m->power_w = (float)v;
            m->has_power = true;
        }
        if (jnum_try(gpu, "temp_c", &v)) {
            m->temp_c = (float)v;
            m->has_temp = true;
        }
        if (jnum_try(gpu, "busy", &v)) {
            m->gpu_pct = (float)v;
            m->has_gpu = true;
        }
        if (jnum_try(gpu, "vram_used_gib", &v)) {
            m->vram_used_mb = (uint32_t)(v * 1024.0);
            m->has_vram = true;
        }
        if (jnum_try(gpu, "vram_total_gib", &v)) {
            m->vram_total_mb = (uint32_t)(v * 1024.0);
            m->has_vram = true;
        }
    }

    cJSON *mem = jpath(r, "mem");
    if (mem) {
        double tot = 0, avail = 0;
        bool has_tot = jnum_try(mem, "total_gib", &tot);
        bool has_avail = jnum_try(mem, "real_available_gib", &avail);
        if (!has_avail) has_avail = jnum_try(mem, "memavailable_gib", &avail);
        if (has_tot) {
            m->ram_total_mb = (uint32_t)(tot * 1024.0);
            m->has_ram = true;
        }
        if (has_tot && has_avail) {
            m->ram_used_mb = (uint32_t)((tot - avail) * 1024.0);
            m->has_ram = true;
        }
    }

    uint32_t sec;
    if (jstr(r, "uptime", buf, sizeof(buf)) && (sec = parse_cn_duration(buf)) != 0) {
        m->host_uptime_s = sec;
        m->has_host_uptime = true;
    }
    if (jstr(r, "container_uptime", buf, sizeof(buf)) &&
        (sec = parse_cn_duration(buf)) != 0) {
        m->model_uptime_s = sec;
        m->has_model_uptime = true;
    }
}

/** 解析面板 /api/status：health（模型/并发）+ metrics（TPS/KV）+ gpu/mem */
static bool parse_panel_status(const char *json, dash_metrics_t *m)
{
    cJSON *r = cJSON_Parse(json);
    if (!r) return false;

    cJSON *h = jpath(r, "health");
    if (h) {
        jstr(h, "model", m->model, sizeof(m->model));
        if (!jstr(h, "version.api", m->version, sizeof(m->version))) {
            jstr(h, "version", m->version, sizeof(m->version));
        }
        m->slots = (uint16_t)jnum(h, "slots", 0);
        m->has_slots = m->slots > 0;
        m->ctx_len = (uint32_t)jnum(h, "context", jnum(h, "slot_ctx", 0));
        double kvp = jnum(h, "kv_pool_positions", 0);
        if (kvp > 0) m->kv_total = (uint32_t)kvp;

        m->in_flight = (uint16_t)jnum(h, "in_flight", 0);
        m->has_concurrency = jpath(h, "in_flight") != NULL;
        m->queued = (uint16_t)jnum(h, "queued", 0);
        m->has_queue = jpath(h, "queued") != NULL;
        m->busy = jbool(h, "busy") || m->in_flight > 0;
    }

    cJSON *mt = jpath(r, "metrics");
    if (mt) {
        double d;
        if (jnum_try(mt, "decode_tps", &d)) {
            m->has_tps = true;
            if (d > 0) {
                s_last_dec = (float)d;
                m->tps_live = true;
            } else {
                m->tps_live = false;   /* 空闲时保留上一次完成请求的速率 */
            }
            m->tps_dec = s_last_dec;
        }
        if (jnum_try(mt, "prefill_tps", &d)) {
            m->has_tps = true;
            if (d > 0) s_last_pre = (float)d;
            m->tps_pre = s_last_pre;
        }
        if (jnum_try(mt, "decode_tps_avg", &d)) m->tps_avg = (float)d;
        if (jnum_try(mt, "prefill_tps_avg", &d)) m->tps_avg_pre = (float)d;

        double peak = jnum(mt, "decode_peak", 0);
        if (peak > s_peak_dec) s_peak_dec = (float)peak;
        note_peak(s_last_dec, s_last_pre);
        m->tps_peak = s_peak_dec;
        m->tps_peak_pre = s_peak_pre;

        if (jnum_try(mt, "kv_used_tokens", &d)) {
            m->kv_used = (uint32_t)d;
            if (m->kv_total > 0) m->kv_pct = (float)(d * 100.0 / m->kv_total);
            m->has_kv = true;
        }
        if (jnum_try(mt, "draft_accept_pct", &d)) {
            m->mtp_pct = (float)d;
            m->has_mtp = true;
        }
        if (jnum_try(mt, "requests_processing", &d)) {
            m->in_flight = (uint16_t)d;
            m->has_concurrency = true;
        }
        if (jnum_try(mt, "requests_deferred", &d)) {
            m->queued = (uint16_t)d;
            m->has_queue = true;
        }
    }

    parse_panel_hw(r, m);

    m->online = true;       /* 面板可达即在线；引擎停了仍能看硬件 */
    cJSON_Delete(r);
    return true;
}

static bool poll_panel(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/api/status", cfg->api_key,
                         s_buf, BUF_BIG, 3000);
    if (n <= 0) return false;
    return parse_panel_status(s_buf, m);
}

/* ============================ strata ============================ */

static int64_t now_epoch(void)
{
    time_t t = time(NULL);
    return (int64_t)t;
}

static bool poll_strata(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/api/monitor", cfg->api_key,
                         s_buf, BUF_BIG, 3000);
    if (n <= 0) return false;

    cJSON *r = cJSON_Parse(s_buf);
    if (!r) return false;

    m->online = true;
    jstr(r, "model", m->model, sizeof(m->model));
    if (!m->model[0]) jstr(r, "engine.model", m->model, sizeof(m->model));
    jstr(r, "version", m->version, sizeof(m->version));

    double v;
    if (jnum_try(r, "outputTps", &v)) {
        m->tps_dec = (float)v;
        m->has_tps = true;
    }
    if (jnum_try(r, "promptTps", &v)) m->tps_pre = (float)v;
    if (jnum_try(r, "averageTps", &v)) m->tps_avg = (float)v;
    if (jnum_try(r, "peakTps", &v)) {
        m->tps_peak = (float)v;
        if ((float)v > s_peak_dec) s_peak_dec = (float)v;
    } else {
        note_peak(m->tps_dec, m->tps_pre);
        m->tps_peak = s_peak_dec;
    }
    m->tps_peak_pre = s_peak_pre;

    if (jnum_try(r, "queue", &v) || jnum_try(r, "queued", &v)) {
        m->queued = (uint16_t)v;
        m->has_queue = true;
    }
    if (jnum_try(r, "inFlight", &v) || jnum_try(r, "activeRequests", &v) ||
        jnum_try(r, "concurrency", &v)) {
        m->in_flight = (uint16_t)v;
        m->has_concurrency = true;
        m->busy = v > 0;
    }
    if (jnum_try(r, "context", &v) || jnum_try(r, "contextLength", &v) ||
        jnum_try(r, "maxContext", &v)) {
        m->ctx_len = (uint32_t)v;
    }
    if (jnum_try(r, "slots", &v)) {
        m->slots = (uint16_t)v;
        m->has_slots = true;
    }
    if (jnum_try(r, "kvUsage", &v)) {
        m->kv_pct = (float)(v <= 1.0 ? v * 100.0 : v);
        m->has_kv = true;
    }

    if (jnum_try(r, "totals.inputTokens", &v) || jnum_try(r, "totals.promptTokens", &v)) {
        m->tokens_in = (uint64_t)v;
        m->has_usage = true;
    }
    if (jnum_try(r, "totals.outputTokens", &v) || jnum_try(r, "totals.generatedTokens", &v)) {
        m->tokens_out = (uint64_t)v;
        m->has_usage = true;
    }
    if (jnum_try(r, "totals.requests", &v) || jnum_try(r, "requests", &v)) {
        m->requests_total = (uint64_t)v;
        m->has_requests = true;
    }

    if (jnum_try(r, "hardware.powerW", &v)) {
        m->power_w = (float)v;
        m->has_power = true;
    }
    if (jnum_try(r, "hardware.gpuTempC", &v) || jnum_try(r, "hardware.tempC", &v)) {
        m->temp_c = (float)v;
        m->has_temp = true;
    }
    if (jnum_try(r, "hardware.gpuLoad", &v)) {
        m->gpu_pct = (float)v;
        m->has_gpu = true;
    }
    if (jnum_try(r, "hardware.vramUsedMb", &v)) {
        m->vram_used_mb = (uint32_t)v;
        m->has_vram = true;
    }
    if (jnum_try(r, "hardware.vramTotalMb", &v)) {
        m->vram_total_mb = (uint32_t)v;
        m->has_vram = true;
    }
    if (jnum_try(r, "hardware.ramUsedMb", &v)) {
        m->ram_used_mb = (uint32_t)v;
        m->has_ram = true;
    }
    if (jnum_try(r, "hardware.ramTotalMb", &v)) {
        m->ram_total_mb = (uint32_t)v;
        m->has_ram = true;
    }

    if (jnum_try(r, "engine.startedAt", &v) && v > 1e9) {
        int64_t up = now_epoch() - (int64_t)v;
        if (up > 0) {
            m->model_uptime_s = (uint32_t)up;
            m->has_model_uptime = true;
        }
    }
    if (jnum_try(r, "uptime", &v) || jnum_try(r, "hostUptime", &v)) {
        m->host_uptime_s = (uint32_t)v;
        m->has_host_uptime = true;
    }

    cJSON_Delete(r);
    return true;
}

/* ============================ LM Studio / Ollama / OpenAI ============================ */

static bool poll_lmstudio(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/api/v0/models", cfg->api_key,
                         s_buf, BUF_BIG, 2500);
    if (n <= 0) return false;

    cJSON *r = cJSON_Parse(s_buf);
    if (!r) return false;

    cJSON *arr = jpath(r, "data");
    if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        cJSON *first = cJSON_GetArrayItem(arr, 0);
        jstr(first, "id", m->model, sizeof(m->model));
        jstr(first, "quantization", m->version, sizeof(m->version));
        double v;
        if (jnum_try(first, "max_context_length", &v) || jnum_try(first, "loaded_context_length", &v)) {
            m->ctx_len = (uint32_t)v;
        }
        if (jnum_try(first, "state", &v)) { /* numeric state, ignore */ }
    }
    m->online = true;
    cJSON_Delete(r);
    return true;
}

static bool poll_ollama(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/api/ps", cfg->api_key,
                         s_buf, BUF_BIG, 2500);
    if (n <= 0) return false;

    cJSON *r = cJSON_Parse(s_buf);
    if (!r) return false;

    cJSON *arr = jpath(r, "models");
    int count = arr ? cJSON_GetArraySize(arr) : 0;
    m->online = true;
    m->busy = count > 0;

    if (count > 0) {
        cJSON *first = cJSON_GetArrayItem(arr, 0);
        jstr(first, "name", m->model, sizeof(m->model));
        jstr(first, "details.quantization_level", m->version, sizeof(m->version));
        double v;
        if (jnum_try(first, "size_vram", &v) && v > 0) {
            m->vram_used_mb = (uint32_t)(v / 1048576.0);
            m->has_vram = true;
        }
        m->slots = (uint16_t)count;
        m->has_slots = true;
        m->in_flight = (uint16_t)count;
        m->has_concurrency = true;
    }

    cJSON_Delete(r);

    char tmp[64];
    n = api_get_path(cfg->server_url, "/api/version", cfg->api_key,
                     s_small, BUF_SMALL, 1500);
    if (n > 0) {
        cJSON *v = cJSON_Parse(s_small);
        if (v) {
            if (jstr(v, "version", tmp, sizeof(tmp))) {
                snprintf(m->version, sizeof(m->version), "%.*s",
                         (int)sizeof(m->version) - 1, tmp);
            }
            cJSON_Delete(v);
        }
    }
    return true;
}

static bool poll_openai(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/v1/models", cfg->api_key,
                         s_buf, BUF_BIG, 2500);
    if (n <= 0) return false;

    cJSON *r = cJSON_Parse(s_buf);
    if (!r) return false;
    cJSON *arr = jpath(r, "data");
    if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        jstr(cJSON_GetArrayItem(arr, 0), "id", m->model, sizeof(m->model));
    }
    m->online = true;
    cJSON_Delete(r);
    return true;
}

/* ============================ 主机监控 agent ============================ */

static void parse_host_stats(cJSON *r, dash_metrics_t *m)
{
    double v;
    if (jnum_try(r, "power_w", &v) || jnum_try(r, "powerW", &v)) {
        m->power_w = (float)v;
        m->has_power = true;
    }
    if (jnum_try(r, "gpu_temp_c", &v) || jnum_try(r, "gpuTempC", &v) ||
        jnum_try(r, "temp_c", &v) || jnum_try(r, "cpu_temp_c", &v)) {
        m->temp_c = (float)v;
        m->has_temp = true;
    }
    if (jnum_try(r, "gpu_pct", &v) || jnum_try(r, "gpuLoad", &v)) {
        m->gpu_pct = (float)v;
        m->has_gpu = true;
    }
    if (jnum_try(r, "vram_used_mb", &v)) {
        m->vram_used_mb = (uint32_t)v;
        m->has_vram = true;
    }
    if (jnum_try(r, "vram_total_mb", &v)) {
        m->vram_total_mb = (uint32_t)v;
        m->has_vram = true;
    }
    if (jnum_try(r, "ram_used_mb", &v)) {
        m->ram_used_mb = (uint32_t)v;
        m->has_ram = true;
    }
    if (jnum_try(r, "ram_total_mb", &v)) {
        m->ram_total_mb = (uint32_t)v;
        m->has_ram = true;
    }
    if (jnum_try(r, "uptime_s", &v)) {
        m->host_uptime_s = (uint32_t)v;
        m->has_host_uptime = true;
    }
    if (jnum_try(r, "model_uptime_s", &v)) {
        m->model_uptime_s = (uint32_t)v;
        m->has_model_uptime = true;
    }
}

/** 主机地址填了面板时，只补齐主数据源缺失的字段（不覆盖已有值） */
static void merge_panel_missing(cJSON *r, dash_metrics_t *m)
{
    double d;

    cJSON *h = jpath(r, "health");
    if (h) {
        if (!m->model[0]) jstr(h, "model", m->model, sizeof(m->model));
        if (!m->has_slots) {
            m->slots = (uint16_t)jnum(h, "slots", 0);
            m->has_slots = m->slots > 0;
        }
        if (m->ctx_len == 0) {
            m->ctx_len = (uint32_t)jnum(h, "context", jnum(h, "slot_ctx", 0));
        }
        if (m->kv_total == 0) {
            double kvp = jnum(h, "kv_pool_positions", 0);
            if (kvp > 0) m->kv_total = (uint32_t)kvp;
        }
    }

    cJSON *mt = jpath(r, "metrics");
    if (!mt) return;

    /* 典型场景：8731 的 /metrics 需要密钥，TPS/KV/MTP 全部借面板补齐 */
    if (!m->has_tps) {
        bool any = false;
        if (jnum_try(mt, "decode_tps", &d)) {
            any = true;
            if (d > 0) {
                s_last_dec = (float)d;
                m->tps_live = true;
            }
        }
        if (jnum_try(mt, "prefill_tps", &d) && d > 0) s_last_pre = (float)d;
        if (jnum_try(mt, "decode_tps_avg", &d)) {
            m->tps_avg = (float)d;
            any = true;
        }
        if (jnum_try(mt, "prefill_tps_avg", &d)) m->tps_avg_pre = (float)d;
        if (any) {
            m->has_tps = true;
            m->tps_dec = s_last_dec;
            m->tps_pre = s_last_pre;
            note_peak(s_last_dec, s_last_pre);
            m->tps_peak = s_peak_dec;
            m->tps_peak_pre = s_peak_pre;
        }
    }
    if (!m->has_kv && jnum_try(mt, "kv_used_tokens", &d)) {
        m->kv_used = (uint32_t)d;
        if (m->kv_total > 0) m->kv_pct = (float)(d * 100.0 / m->kv_total);
        m->has_kv = true;
    }
    if (!m->has_mtp && jnum_try(mt, "draft_accept_pct", &d)) {
        m->mtp_pct = (float)d;
        m->has_mtp = true;
    }
    if (!m->has_concurrency && jnum_try(mt, "requests_processing", &d)) {
        m->in_flight = (uint16_t)d;
        m->has_concurrency = true;
    }
    if (!m->has_queue && jnum_try(mt, "requests_deferred", &d)) {
        m->queued = (uint16_t)d;
        m->has_queue = true;
    }
}

static void poll_host_agent(const app_config_t *cfg, dash_metrics_t *m)
{
    if (!cfg->host_agent[0]) return;

    char url[200];
    int n;

    /* 1) 约定接口：host_agent.py 的 /stats */
    if (strstr(cfg->host_agent, "/stats")) {
        snprintf(url, sizeof(url), "%s", cfg->host_agent);
    } else {
        snprintf(url, sizeof(url), "%s/stats", cfg->host_agent);
    }
    n = api_get(url, NULL, s_small, BUF_SMALL, 2000);
    if (n > 0) {
        cJSON *r = cJSON_Parse(s_small);
        if (r) {
            parse_host_stats(r, m);
            cJSON_Delete(r);
            return;
        }
    }

    /* 2) 预留地址填 halogen-panel：直接读 /api/status 的硬件段 */
    if (strstr(cfg->host_agent, "/api/status")) {
        snprintf(url, sizeof(url), "%s", cfg->host_agent);
    } else {
        snprintf(url, sizeof(url), "%s/api/status", cfg->host_agent);
    }
    n = api_get(url, NULL, s_buf, BUF_BIG, 2500);
    if (n <= 0 || !strstr(s_buf, "\"container_uptime\"")) return;

    cJSON *r = cJSON_Parse(s_buf);
    if (!r) return;
    parse_panel_hw(r, m);
    merge_panel_missing(r, m);
    cJSON_Delete(r);
}

/* ============================ 环境信息 ============================ */

static void fill_env(const app_config_t *cfg, dash_metrics_t *m)
{
    snprintf(m->wifi_ssid, sizeof(m->wifi_ssid), "%s", cfg->wifi_ssid);
    wifi_mgr_sta_ip(m->wifi_ip, sizeof(m->wifi_ip));
    m->wifi_rssi = wifi_mgr_rssi();
    m->free_heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
    m->device_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    /* 屏幕里显示的是去掉协议头的地址，更省空间 */
    const char *ep = cfg->server_url;
    if (strncmp(ep, "http://", 7) == 0) ep += 7;
    else if (strncmp(ep, "https://", 8) == 0) ep += 8;
    snprintf(m->endpoint, sizeof(m->endpoint), "%.*s",
             (int)sizeof(m->endpoint) - 1, ep);

    const char *ha = cfg->host_agent;
    if (strncmp(ha, "http://", 7) == 0) ha += 7;
    else if (strncmp(ha, "https://", 8) == 0) ha += 8;
    snprintf(m->host_agent, sizeof(m->host_agent), "%.*s",
             (int)sizeof(m->host_agent) - 1, ha);

    /* API key 脱敏 */
    size_t kl = strlen(cfg->api_key);
    if (kl == 0) {
        snprintf(m->api_key_mask, sizeof(m->api_key_mask), "未设置");
    } else if (kl <= 10) {
        snprintf(m->api_key_mask, sizeof(m->api_key_mask), "已设置");
    } else {
        snprintf(m->api_key_mask, sizeof(m->api_key_mask), "%.6s…%.4s",
                 cfg->api_key, cfg->api_key + kl - 4);
    }

    snprintf(m->provider_name, sizeof(m->provider_name), "%s", provider_name(s_prov));
    m->provider = s_prov;
}

/* ============================ 对外 ============================ */

void metrics_init(void)
{
    s_buf = heap_caps_malloc(BUF_BIG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_small = heap_caps_malloc(BUF_SMALL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || !s_small) {
        ESP_LOGE(TAG, "no PSRAM for HTTP buffers");
    }
    s_prov = DASH_PROV_UNKNOWN;
}

bool metrics_poll(const app_config_t *cfg, dash_metrics_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!s_buf || !s_small) return false;

    /* 没连上 Wi-Fi 就别发请求：既省时间，也避免探测被反复打断 */
    if (!wifi_mgr_sta_connected()) goto failed;

    bool explicit_provider = strcmp(cfg->provider, "auto") != 0;
    if (strcmp(s_base, cfg->server_url) != 0 ||
        strcmp(s_key, cfg->api_key) != 0 ||
        strcmp(s_provider, cfg->provider) != 0) {
        snprintf(s_base, sizeof(s_base), "%s", cfg->server_url);
        snprintf(s_key, sizeof(s_key), "%s", cfg->api_key);
        snprintf(s_provider, sizeof(s_provider), "%s", cfg->provider);
        s_probed = false;
        s_diff_primed = false;
    }

    if (explicit_provider) {
        s_prov = provider_from_name(cfg->provider);
        s_probed = true;
    } else if (!s_probed) {
        s_probed = probe(cfg);
    }

    if (!s_probed) {
        goto failed;
    }

    bool ok = false;
    switch (s_prov) {
    case DASH_PROV_HALOGEN:
    case DASH_PROV_GUFO:
        ok = poll_llamacpp(cfg, out);
        break;
    case DASH_PROV_STRATA:
        ok = poll_strata(cfg, out);
        break;
    case DASH_PROV_LMSTUDIO:
        ok = poll_lmstudio(cfg, out);
        break;
    case DASH_PROV_OLLAMA:
        ok = poll_ollama(cfg, out);
        break;
    case DASH_PROV_OPENAI:
        ok = poll_openai(cfg, out);
        break;
    case DASH_PROV_PANEL:
        ok = poll_panel(cfg, out);
        break;
    default:
        break;
    }

    if (!ok) goto failed;

    s_fail = 0;
    if (out->tps_peak < s_peak_dec) out->tps_peak = s_peak_dec;
    if (out->tps_peak_pre < s_peak_pre) out->tps_peak_pre = s_peak_pre;
    out->stale = false;
    out->online = true;

    poll_host_agent(cfg, out);
    fill_env(cfg, out);

    s_last = *out;
    s_have_last = true;
    return true;

failed:
    s_fail++;
    if (s_fail >= 3) s_probed = false;

    if (s_have_last) {
        *out = s_last;
        out->online = false;
        out->stale = true;
        out->busy = false;
        out->tps_live = false;
        fill_env(cfg, out);
    } else {
        fill_env(cfg, out);
        out->online = false;
        out->stale = true;
    }
    return false;
}

void metrics_publish(const dash_metrics_t *m)
{
    if (!m) return;

    dash_metric_store_set_bool("llm.online", m->online, true);
    dash_metric_store_set_bool("llm.busy", m->busy, true);
    if (m->model[0]) dash_metric_store_set_string("llm.model", m->model, true);
    if (m->version[0]) dash_metric_store_set_string("llm.version", m->version, true);

    if (m->has_tps) {
        dash_metric_store_set_number("llm.decode_tps", m->tps_dec, true);
        dash_metric_store_set_number("llm.prefill_tps", m->tps_pre, true);
        dash_metric_store_set_number("llm.decode_peak", m->tps_peak, true);
        dash_metric_store_set_number("llm.prefill_peak", m->tps_peak_pre, true);
        if (m->tps_avg > 0) {
            dash_metric_store_set_number("llm.decode_avg", m->tps_avg, true);
        }
        if (m->tps_avg_pre > 0) {
            dash_metric_store_set_number("llm.prefill_avg", m->tps_avg_pre, true);
        }
    }
    if (m->has_concurrency) {
        dash_metric_store_set_number("llm.requests_processing", m->in_flight, true);
    }
    if (m->has_queue) {
        dash_metric_store_set_number("llm.requests_deferred", m->queued, true);
    }
    if (m->has_slots) {
        dash_metric_store_set_number("llm.slots", m->slots, true);
    }
    if (m->ctx_len > 0) {
        dash_metric_store_set_number("llm.context_len", m->ctx_len, true);
    }
    if (m->kv_total > 0) {
        dash_metric_store_set_number("llm.kv_pool_positions", m->kv_total, true);
    }
    if (m->has_kv) {
        dash_metric_store_set_number("llm.kv_used_tokens", m->kv_used, true);
        dash_metric_store_set_number("llm.kv_pct", m->kv_pct, true);
    }
    if (m->tokens_cached > 0) {
        dash_metric_store_set_number("llm.cache_hit_tokens", (double)m->tokens_cached, true);
    }
    if (m->cache_bytes > 0) {
        dash_metric_store_set_number("llm.cache_cap_mb",
                                     (double)m->cache_bytes / (1024.0 * 1024.0), true);
        dash_metric_store_set_bool("llm.cache_enabled", true, true);
    }
    if (m->has_model_uptime) {
        dash_metric_store_set_number("llm.uptime_s", m->model_uptime_s, true);
    }
    if (m->has_host_uptime) {
        dash_metric_store_set_number("host.uptime_s", m->host_uptime_s, true);
    }

    if (m->has_power) {
        dash_metric_store_set_number("host.gpu_power_w", m->power_w, true);
    }
    if (m->has_temp) {
        dash_metric_store_set_number("host.gpu_temp_c", m->temp_c, true);
    }
    if (m->has_gpu) {
        dash_metric_store_set_bool("host.gpu_available", true, true);
        dash_metric_store_set_number("host.gpu_util_pct", m->gpu_pct, true);
    }
    if (m->has_vram) {
        dash_metric_store_set_number("host.gpu_vram_used_mb", m->vram_used_mb, true);
        dash_metric_store_set_number("host.gpu_vram_total_mb", m->vram_total_mb, true);
    }
    if (m->has_ram) {
        dash_metric_store_set_number("host.ram_used_mb", m->ram_used_mb, true);
        dash_metric_store_set_number("host.ram_total_mb", m->ram_total_mb, true);
    }
}

void metrics_reset_peak(void)
{
    s_peak_dec = 0;
    s_peak_pre = 0;
    if (s_have_last) {
        s_last.tps_peak = 0;
        s_last.tps_peak_pre = 0;
    }
}

dash_provider_t metrics_provider(void)
{
    return s_prov;
}
