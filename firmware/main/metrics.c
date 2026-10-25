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
static bool s_last_ok;
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

static const char *provider_key(dash_provider_t p)
{
    switch (p) {
    case DASH_PROV_HALOGEN: return "halogen";
    case DASH_PROV_GUFO:    return "gufo";
    case DASH_PROV_STRATA:  return "strata";
    case DASH_PROV_LMSTUDIO:return "lmstudio";
    case DASH_PROV_OLLAMA:  return "ollama";
    case DASH_PROV_OPENAI:  return "openai";
    case DASH_PROV_PANEL:   return "panel";
    default:                return "unknown";
    }
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

    /* Strata：/health 返回 "service":"strata"，且不要求密钥，辨识最稳 */
    n = api_get_path(base, "/health", key, s_small, BUF_SMALL, 2000);
    if (n > 0) {
        cJSON *h = cJSON_Parse(s_small);
        if (h) {
            char svc[16];
            bool is_strata = jstr(h, "service", svc, sizeof(svc)) &&
                             strcmp(svc, "strata") == 0;
            cJSON_Delete(h);
            if (is_strata) {
                s_prov = DASH_PROV_STRATA;
                ESP_LOGI(TAG, "detected strata");
                return true;
            }
        }
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

/* 峰值只在 strata / panel 路径累计：halogen 的本地峰值不是 API 原始值，不入册 */
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

    /* 排队时长与引擎探活：这几个键在 halogen /health 里总是一起出现 */
    if (jpath(r, "busy_for_s") || jpath(r, "engine.responds")) {
        m->has_engine_state = true;
        m->busy_for_s = (float)jnum(r, "busy_for_s", 0);
        m->longest_wait_s = (float)jnum(r, "longest_wait_s", 0);
        m->engine_responds = jbool(r, "engine.responds");
        m->engine_probe_s = (float)jnum(r, "engine.probe_s", 0);
    }
    jstr(r, "capability_probe", m->capability_probe, sizeof(m->capability_probe));

    /* 能力声明：只有 halogen 会自报，取不到就整组留空 */
    if (jpath(r, "vision.enabled") || jpath(r, "max_tokens_default")) {
        m->has_capabilities = true;
        m->max_tokens_default = (uint32_t)jnum(r, "max_tokens_default", 0);
        m->indexer_budget = (uint32_t)jnum(r, "indexer_budget", 0);
        m->vision_enabled = jbool(r, "vision.enabled");
        m->structured_output = jbool(r, "structured_output.enabled");
    }
    if (jpath(r, "drafter_default") || jpath(r, "drafter_weights_loaded")) {
        m->has_drafter = true;
        jstr(r, "drafter_default", m->drafter, sizeof(m->drafter));
        m->drafter_loaded = jbool(r, "drafter_weights_loaded");
    }
    if (jpath(r, "prompt_cache")) {
        m->has_cache = true;
        m->cache_enabled = jbool(r, "prompt_cache.enabled");
        m->cache_cap_mb = (uint32_t)jnum(r, "prompt_cache.cap_mb", 0);
    }

    m->online = true;
    cJSON_Delete(r);
    return true;
}

/** halogen 专属：/cache 给命中率与 KV 池细分（响应约 1.2KB）；失败静默忽略 */
static void poll_halogen_cache(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/cache", cfg->api_key,
                         s_small, BUF_SMALL, 2500);
    if (n <= 0) return;

    cJSON *r = cJSON_Parse(s_small);
    if (!r) return;

    double v;
    if (jnum_try(r, "hit_rate", &v)) {
        m->cache_hit_rate = (float)(v * 100.0);
        m->has_cache_rates = true;
    }
    if (jnum_try(r, "token_hit_rate", &v)) {
        m->cache_token_hit_rate = (float)(v * 100.0);
        m->has_cache_rates = true;
    }

    cJSON *pool = jpath(r, "pool");
    if (pool) {
        m->has_kv_detail = true;
        m->kv_busy_regions = (uint32_t)jnum(pool, "busy_regions", 0);
        m->kv_held_regions = (uint32_t)jnum(pool, "held_regions", 0);
        m->kv_waiting_for_room = (uint32_t)jnum(pool, "waiting_for_room", 0);
        if (m->kv_total == 0 && jnum_try(pool, "positions", &v) && v > 0) {
            m->kv_total = (uint32_t)v;
        }
    }
    cJSON_Delete(r);
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

        if (mval(s_buf, "llamacpp:requests_processing", &v)) {
            m->in_flight = (uint16_t)v;
            m->has_concurrency = true;
        }
        if (mval(s_buf, "llamacpp:requests_deferred", &v)) {
            m->queued = (uint16_t)v;
            m->has_queue = true;
        }
        if (mval(s_buf, "halogen:queue_waited_total", &v)) {
            m->queue_waited = (uint32_t)v;
            m->has_queue_counters = true;
        }
        if (mval(s_buf, "halogen:queue_gave_up_total", &v)) {
            m->queue_gave_up = (uint32_t)v;
            m->has_queue_counters = true;
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
        if (mval(s_buf, "halogen:kv_pool_reserved_tokens", &v)) {
            m->kv_reserved = (uint32_t)v;
            m->has_kv_detail = true;
        }
        if (mval(s_buf, "halogen:kv_pool_waiting_for_room", &v)) {
            m->kv_waiting_for_room = (uint32_t)v;
            m->has_kv_detail = true;
        }

        if (mval(s_buf, "llamacpp:prompt_tokens_total", &v)) {
            m->tokens_in = (uint64_t)v;
            m->has_usage = true;
        }
        if (mval(s_buf, "llamacpp:tokens_predicted_total", &v)) {
            m->tokens_out = (uint64_t)v;
            m->has_usage = true;
        }
        v = 0;
        if (!mval(s_buf, "halogen:cache_prompt_tokens_saved_total", &v)) {
            mval(s_buf, "halogen:prompt_tokens_cached_total", &v);
        }
        m->tokens_cached = (uint64_t)v;
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
                /* /cache 不可用（缺密钥）时，用命中/未命中计数兜底同一个口径 */
                m->cache_hit_rate = (float)(hits * 100.0 / tot);
                m->has_cache_rates = true;
            }
        }
        if (mval(s_buf, "halogen:cache_bytes", &bytes)) {
            m->cache_bytes = (uint64_t)bytes;
        }
        if (mval(s_buf, "halogen:cache_entries", &entries)) {
            m->cache_entries = (uint32_t)entries;
        }
        if (mval(s_buf, "halogen:cache_evicted_total", &v)) {
            m->cache_evicted = (uint32_t)v;
        }

        double draft, accepted;
        if (mval(s_buf, "halogen:draft_tokens_total", &draft)) {
            m->draft_tokens = (uint64_t)draft;
            if (draft > 0 &&
                mval(s_buf, "halogen:draft_tokens_accepted_total", &accepted)) {
                m->mtp_pct = (float)(accepted * 100.0 / draft);
                m->has_mtp = true;
            }
        }
    }

    /* /cache 在 /metrics 之后：它给的命中率是权威口径，覆盖上面的计数兜底值 */
    if (s_prov == DASH_PROV_HALOGEN) {
        poll_halogen_cache(cfg, m);
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
                } else {
                    m->tps_live = false;
                }
                s_gufo_decoded = sum;
                s_gufo_us = now;

                m->tps_dec = s_last_dec;
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

/** 面板硬件段（gpu/mem）+ 容器运行时长 → 统一指标；host_agent 复用 */
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

    /* 主机 CPU：只有 panel / strata 的接口带 */
    if (jnum_try(r, "cpu_percent", &v)) {
        m->cpu_pct = (float)v;
        m->has_cpu = true;
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

    /* 主机 uptime 已不在测点目录，只保留容器（引擎）运行时长 */
    uint32_t sec;
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

        /* 面板透传 halogen /health，字段口径与直连一致 */
        if (jpath(h, "busy_for_s") || jpath(h, "engine.responds")) {
            m->has_engine_state = true;
            m->busy_for_s = (float)jnum(h, "busy_for_s", 0);
            m->longest_wait_s = (float)jnum(h, "longest_wait_s", 0);
            m->engine_responds = jbool(h, "engine.responds");
            m->engine_probe_s = (float)jnum(h, "engine.probe_s", 0);
        }
        if (jpath(h, "drafter_default") || jpath(h, "drafter_weights_loaded")) {
            m->has_drafter = true;
            jstr(h, "drafter_default", m->drafter, sizeof(m->drafter));
            m->drafter_loaded = jbool(h, "drafter_weights_loaded");
        }
        if (jpath(h, "prompt_cache")) {
            m->has_cache = true;
            m->cache_enabled = jbool(h, "prompt_cache.enabled");
            m->cache_cap_mb = (uint32_t)jnum(h, "prompt_cache.cap_mb", 0);
        }
        /* 面板同样透传引擎能力声明，取到就视为有效 */
        if (jpath(h, "max_tokens_default") || jpath(h, "indexer_budget")) {
            m->has_capabilities = true;
            m->max_tokens_default = (uint32_t)jnum(h, "max_tokens_default", 0);
            m->indexer_budget = (uint32_t)jnum(h, "indexer_budget", 0);
            m->vision_enabled = jbool(h, "vision.enabled");
            m->structured_output = jbool(h, "structured_output.enabled");
        }
        if (jstr(h, "capability_probe", m->capability_probe,
                 sizeof(m->capability_probe))) {
            m->has_engine_state = true;
        }
    }

    cJSON *mt = jpath(r, "metrics");
    if (mt) {
        double d;
        jstr(mt, "phase", m->phase, sizeof(m->phase));
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
        peak = jnum(mt, "prefill_peak", 0);
        if (peak > s_peak_pre) s_peak_pre = (float)peak;
        note_peak(s_last_dec, s_last_pre);
        m->tps_peak = s_peak_dec;
        m->tps_peak_pre = s_peak_pre;

        if (jnum_try(mt, "kv_used_tokens", &d)) {
            m->kv_used = (uint32_t)d;
            if (m->kv_total > 0) m->kv_pct = (float)(d * 100.0 / m->kv_total);
            m->has_kv = true;
        }
        if (jnum_try(mt, "cache_hit_tokens", &d)) {
            m->tokens_cached = (uint64_t)d;
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

/* Strata 官方接口（docs/DETAILS.md#using-it）：
 *   /v1/status  模型、版本、上下文、并发、uptime、GPU/内存
 *   /metrics    JSON，实时 tok/s、队列、缓存命中；带 60 点 history 与最近
 *               请求列表，可能超出 BUF_BIG，解析失败时降级为只用 /v1/status。
 * 注意不要用 /metrics/prometheus：那个端点不含主机硬件指标。 */
static bool poll_strata(const app_config_t *cfg, dash_metrics_t *m)
{
    int n = api_get_path(cfg->server_url, "/v1/status", cfg->api_key,
                         s_buf, BUF_BIG, 3000);
    if (n <= 0) return false;

    cJSON *st = cJSON_Parse(s_buf);
    if (!st) return false;

    m->online = true;
    jstr(st, "model", m->model, sizeof(m->model));
    jstr(st, "engine", m->version, sizeof(m->version));

    double v;
    if (jnum_try(st, "context.native", &v) && v > 0) {
        m->ctx_len = (uint32_t)v;
    } else if (jnum_try(st, "cache_max_tokens", &v) && v > 0) {
        m->ctx_len = (uint32_t)v;
    }

    if (jnum_try(st, "concurrency.serving", &v)) {
        m->slots = (uint16_t)v;
        m->has_slots = true;
    }
    if (jnum_try(st, "activity.in_flight", &v)) {
        m->in_flight = (uint16_t)v;
        m->has_concurrency = true;
        m->busy = v > 0;
    }
    if (jnum_try(st, "uptime_s", &v) && v > 0) {
        m->model_uptime_s = (uint32_t)v;
        m->has_model_uptime = true;
    }

    /* 主机硬件：strata 是少数自带硬件指标的引擎 */
    if (jnum_try(st, "machine.gpu.util_pct", &v)) {
        m->gpu_pct = (float)v;
        m->has_gpu = true;
    }
    if (jnum_try(st, "machine.gpu.temp_c", &v)) {
        m->temp_c = (float)v;
        m->has_temp = true;
    }
    if (jnum_try(st, "machine.gpu.power_w", &v)) {
        m->power_w = (float)v;
        m->has_power = true;
    }
    if (jnum_try(st, "machine.gpu.used_mib", &v)) {
        m->vram_used_mb = (uint32_t)v;
        m->has_vram = true;
    }
    if (jnum_try(st, "machine.gpu.total_mib", &v)) {
        m->vram_total_mb = (uint32_t)v;
        m->has_vram = true;
    }
    if (jnum_try(st, "machine.ram.used_gib", &v)) {
        m->ram_used_mb = (uint32_t)(v * 1024.0);
        m->has_ram = true;
    }
    if (jnum_try(st, "machine.ram.total_gib", &v)) {
        m->ram_total_mb = (uint32_t)(v * 1024.0);
        m->has_ram = true;
    }

    /* last_timings 沿用 llama.cpp 命名，可当最近一次请求的速度 */
    if (jnum_try(st, "last_timings.predicted_per_second", &v) && v > 0) {
        m->tps_dec = (float)v;
        m->has_tps = true;
    }
    if (jnum_try(st, "last_timings.prompt_per_second", &v) && v > 0) {
        m->tps_pre = (float)v;
    }

    cJSON_Delete(st);

    /* 第二段：/metrics 只补实时速率、队列与缓存；失败不影响在线状态 */
    n = api_get_path(cfg->server_url, "/metrics", cfg->api_key,
                     s_buf, BUF_BIG, 3000);
    if (n > 0) {
        cJSON *mt = cJSON_Parse(s_buf);
        if (mt) {
            char state[24] = "";
            jstr(mt, "live.state", state, sizeof(state));
            jstr(mt, "live.phase", m->phase, sizeof(m->phase));
            if (!m->phase[0]) {
                snprintf(m->phase, sizeof(m->phase), "%s", state);
            }

            bool busy_state = strcmp(state, "reading") == 0 ||
                              strcmp(state, "generating") == 0;

            if (jnum_try(mt, "live.tok_s", &v) && v > 0) {
                m->tps_dec = (float)v;
                m->has_tps = true;
                m->tps_live = true;
            }
            if (jnum_try(mt, "live.prefill_tok_s_mean", &v) && v > 0) {
                m->tps_pre = (float)v;
                m->tps_avg_pre = (float)v;
            }
            if (jnum_try(mt, "live.tok_s_mean", &v) && v > 0) {
                m->tps_avg = (float)v;
            }

            double queued = 0, waiting = 0;
            bool has_queued = jnum_try(mt, "live.queued", &queued);
            bool has_waiting = jnum_try(mt, "live.waiting", &waiting);
            if (has_queued || has_waiting) {
                m->queued = (uint16_t)(queued + waiting);
                m->has_queue = true;
            }

            if (jnum_try(mt, "live.running", &v)) {
                m->in_flight = (uint16_t)v;
                m->has_concurrency = true;
            }
            if (busy_state || m->in_flight > 0) m->busy = true;

            /* KV 占用用 strata 自己的口径：在途 prompt + 生成 占 context 的比例。
             * 空闲时没有在途请求，保持不可用，避免曲线假装有占用。 */
            double prompt_tokens = 0, generated = 0;
            if (busy_state && m->ctx_len > 0 &&
                jnum_try(mt, "live.prompt_tokens", &prompt_tokens)) {
                jnum_try(mt, "live.generated", &generated);
                double used = prompt_tokens + generated;
                m->kv_used = (uint32_t)used;
                m->kv_pct = (float)(used * 100.0 / (double)m->ctx_len);
                if (m->kv_pct > 100.0f) m->kv_pct = 100.0f;
                m->has_kv = true;
            }

            if (jnum_try(mt, "totals.reused", &v) && v > 0) {
                m->tokens_cached = (uint64_t)v;
            }
            if (jnum_try(mt, "totals.prompt_tokens", &v)) {
                m->tokens_in = (uint64_t)v;
                m->has_usage = true;
            }
            if (jnum_try(mt, "totals.output_tokens", &v)) {
                m->tokens_out = (uint64_t)v;
                m->has_usage = true;
            }
            if (jnum_try(mt, "totals.requests", &v)) {
                m->requests_total = (uint64_t)v;
                m->has_requests = true;
            }
            if (jnum_try(mt, "conversation_cache.budget_mib", &v) && v > 0) {
                m->cache_bytes = (uint64_t)(v * 1024.0 * 1024.0);
            }

            cJSON_Delete(mt);
        } else {
            /* /metrics 被截断（history + requests 太大），本轮只用 /v1/status */
            ESP_LOGD(TAG, "strata /metrics too large or invalid, status only");
        }
    }

    note_peak(m->tps_dec, m->tps_pre);
    m->tps_peak = s_peak_dec;
    m->tps_peak_pre = s_peak_pre;
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
    if (jnum_try(r, "model_uptime_s", &v)) {
        m->model_uptime_s = (uint32_t)v;
        m->has_model_uptime = true;
    }
    /* 主机 uptime 已不在测点目录，agent 即使上报也不再接收 */
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
    /* 不要在轮询开始时清 s_last_ok：一次轮询要几百毫秒到 1.5 秒，
     * WS status 会在这段时间读到 false，表现为 api_ok 每秒闪烁。
     * 只有本次轮询真正失败时才在 failed 分支置 false。 */

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
    s_last_ok = true;
    return true;

failed:
    s_last_ok = false;
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

/* 只发布 shared/catalog/metrics.json（version 2）里在册的 id：
 * 服务端 MetricStore.update() 对未注册 id 抛 KeyError，固件侧同样宁缺勿滥。
 * 已下线的 host.uptime_s / disk / npu / tts / embed / lan_ip / cpu_freq 等不再采集。 */
void metrics_publish(const dash_metrics_t *m)
{
    if (!m) return;

    bool peak_from_api = m->provider == DASH_PROV_STRATA ||
                         m->provider == DASH_PROV_PANEL;

    dash_metric_store_set_bool("llm.online", m->online, true);
    dash_metric_store_set_bool("llm.busy", m->busy, true);
    if (m->model[0]) dash_metric_store_set_string("llm.model", m->model, true);
    if (m->version[0]) dash_metric_store_set_string("llm.version", m->version, true);
    if (m->phase[0]) dash_metric_store_set_string("llm.phase", m->phase, true);

    if (m->has_engine_state) {
        dash_metric_store_set_number("llm.busy_for_s", m->busy_for_s, true);
        dash_metric_store_set_number("llm.longest_wait_s", m->longest_wait_s, true);
        dash_metric_store_set_bool("llm.engine_responds", m->engine_responds, true);
        dash_metric_store_set_number("llm.engine_probe_s", m->engine_probe_s, true);
    }
    if (m->capability_probe[0]) {
        dash_metric_store_set_string("llm.capability_probe", m->capability_probe, true);
    }
    if (m->has_model_uptime) {
        dash_metric_store_set_number("llm.uptime_s", m->model_uptime_s, true);
    }

    if (m->has_tps) {
        dash_metric_store_set_number("llm.decode_tps", m->tps_dec, true);
        dash_metric_store_set_number("llm.prefill_tps", m->tps_pre, true);
        /* 峰值只有 strata / panel 的 API 才原始交付，halogen 本地累计的不发布 */
        if (peak_from_api) {
            dash_metric_store_set_number("llm.decode_peak", m->tps_peak, true);
            dash_metric_store_set_number("llm.prefill_peak", m->tps_peak_pre, true);
        }
        if (m->tps_avg > 0) {
            dash_metric_store_set_number("llm.decode_avg", m->tps_avg, true);
        }
        if (m->tps_avg_pre > 0) {
            dash_metric_store_set_number("llm.prefill_avg", m->tps_avg_pre, true);
        }
    }
    if (m->has_usage) {
        dash_metric_store_set_number("llm.tokens_in", (double)m->tokens_in, true);
        dash_metric_store_set_number("llm.tokens_out", (double)m->tokens_out, true);
    }
    if (m->has_requests) {
        dash_metric_store_set_number("llm.requests_total", (double)m->requests_total, true);
    }

    if (m->has_concurrency) {
        dash_metric_store_set_number("llm.requests_processing", m->in_flight, true);
    }
    if (m->has_queue) {
        dash_metric_store_set_number("llm.requests_deferred", m->queued, true);
    }
    if (m->has_queue_counters) {
        dash_metric_store_set_number("llm.queue_waited", m->queue_waited, true);
        dash_metric_store_set_number("llm.queue_gave_up", m->queue_gave_up, true);
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
    if (m->has_kv_detail) {
        dash_metric_store_set_number("llm.kv_reserved_tokens", m->kv_reserved, true);
        dash_metric_store_set_number("llm.kv_busy_regions", m->kv_busy_regions, true);
        dash_metric_store_set_number("llm.kv_held_regions", m->kv_held_regions, true);
        dash_metric_store_set_number("llm.kv_waiting_for_room",
                                     m->kv_waiting_for_room, true);
    }

    if (m->has_cache) {
        dash_metric_store_set_bool("llm.cache_enabled", m->cache_enabled, true);
    }
    if (m->cache_cap_mb > 0) {
        dash_metric_store_set_number("llm.cache_cap_mb", m->cache_cap_mb, true);
    }
    if (m->has_cache_rates) {
        dash_metric_store_set_number("llm.cache_hit_rate", m->cache_hit_rate, true);
        dash_metric_store_set_number("llm.cache_token_hit_rate",
                                     m->cache_token_hit_rate, true);
    }
    if (m->tokens_cached > 0) {
        dash_metric_store_set_number("llm.cache_hit_tokens", (double)m->tokens_cached, true);
    }
    if (m->cache_entries > 0) {
        dash_metric_store_set_number("llm.cache_entries", m->cache_entries, true);
    }
    if (m->cache_bytes > 0) {
        dash_metric_store_set_number("llm.cache_bytes",
                                     (double)m->cache_bytes / (1024.0 * 1024.0), true);
    }
    if (m->cache_evicted > 0) {
        dash_metric_store_set_number("llm.cache_evicted", m->cache_evicted, true);
    }

    if (m->has_mtp) {
        dash_metric_store_set_number("llm.draft_accept_pct", m->mtp_pct, true);
    }
    if (m->draft_tokens > 0) {
        dash_metric_store_set_number("llm.draft_tokens", (double)m->draft_tokens, true);
    }
    if (m->drafter[0]) {
        dash_metric_store_set_string("llm.drafter", m->drafter, true);
    }
    if (m->has_drafter) {
        dash_metric_store_set_bool("llm.drafter_loaded", m->drafter_loaded, true);
    }

    if (m->has_capabilities) {
        if (m->max_tokens_default > 0) {
            dash_metric_store_set_number("llm.max_tokens_default",
                                         m->max_tokens_default, true);
        }
        if (m->indexer_budget > 0) {
            dash_metric_store_set_number("llm.indexer_budget",
                                         m->indexer_budget, true);
        }
        dash_metric_store_set_bool("llm.vision_enabled", m->vision_enabled, true);
        dash_metric_store_set_bool("llm.structured_output", m->structured_output, true);
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
    if (m->has_cpu) {
        dash_metric_store_set_number("host.cpu_pct", m->cpu_pct, true);
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

const char *metrics_provider_name(void)
{
    return provider_name(s_prov);
}

const char *metrics_provider_key(void)
{
    return provider_key(s_prov);
}

bool metrics_last_ok(void)
{
    return s_last_ok;
}
