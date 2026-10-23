#include "dash_metric_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "mstore";

static dash_metric_t *s_metrics;
static int s_count;

bool dash_metric_store_init(void)
{
    if (s_metrics) return true;
    s_metrics = heap_caps_calloc(DASH_METRIC_MAX, sizeof(dash_metric_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_metrics) {
        ESP_LOGE(TAG, "no PSRAM for metric store");
        return false;
    }
    s_count = 0;
    ESP_LOGI(TAG, "metric store ready: %d slots, %u bytes",
             DASH_METRIC_MAX, (unsigned)(DASH_METRIC_MAX * sizeof(dash_metric_t)));
    return true;
}

dash_metric_t *dash_metric_store_find(const char *id)
{
    if (!s_metrics || !id) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_metrics[i].id, id) == 0) return &s_metrics[i];
    }
    return NULL;
}

dash_metric_t *dash_metric_store_get(const char *id)
{
    if (!s_metrics || !id || !id[0]) return NULL;
    dash_metric_t *m = dash_metric_store_find(id);
    if (m) return m;
    if (s_count >= DASH_METRIC_MAX) return NULL;
    m = &s_metrics[s_count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->id, sizeof(m->id), "%s", id);
    m->type = DASH_METRIC_NUMBER;
    m->available = false;
    return m;
}

void dash_metric_store_set_number(const char *id, double value, bool available)
{
    dash_metric_t *m = dash_metric_store_get(id);
    if (!m) return;
    m->type = DASH_METRIC_NUMBER;
    m->number = value;
    m->available = available;
}

void dash_metric_store_set_bool(const char *id, bool value, bool available)
{
    dash_metric_t *m = dash_metric_store_get(id);
    if (!m) return;
    m->type = DASH_METRIC_BOOL;
    m->boolean = value;
    m->available = available;
}

void dash_metric_store_set_string(const char *id, const char *value, bool available)
{
    dash_metric_t *m = dash_metric_store_get(id);
    if (!m) return;
    m->type = DASH_METRIC_STRING;
    snprintf(m->text, sizeof(m->text), "%s", value ? value : "");
    m->available = available && value != NULL;
}

void dash_metric_store_mark_unavailable(const char *id)
{
    dash_metric_t *m = dash_metric_store_get(id);
    if (m) m->available = false;
}

void dash_metric_store_dump(void)
{
    if (!s_metrics) {
        printf("store not ready\n");
        return;
    }
    printf("metric store: %d entries\n", s_count);
    for (int i = 0; i < s_count; i++) {
        dash_metric_t *m = &s_metrics[i];
        if (m->type == DASH_METRIC_NUMBER) {
            printf("  %-30s a=%d type=num value=%.3f\n",
                   m->id, m->available, m->number);
        } else if (m->type == DASH_METRIC_BOOL) {
            printf("  %-30s a=%d type=bool value=%d\n",
                   m->id, m->available, m->boolean);
        } else {
            printf("  %-30s a=%d type=str value=%s\n",
                   m->id, m->available, m->text);
        }
    }
}
