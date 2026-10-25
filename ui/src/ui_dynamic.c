#include "ui_dynamic.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dash_metric_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_classic.h"
#include "ui_mini.h"
#include "ui_pet.h"

static const char *TAG = "uidyn";

#define DYN_MAX_WIDGETS 32
#define DYN_MAX_BINDINGS 8
#define DYN_MAX_ROWS 8
#define DYN_MAX_SUB_IDS 64

typedef struct {
    int w;
    int h;
    int cols;
    int rows;
    int gap;
    int pad;
} dyn_canvas_t;

/* 默认竖屏；构建布局时按 profile/canvas 更新为当前方向 */
static dyn_canvas_t s_canvas = {320, 480, 12, 18, 8, 4};

enum {
    DYN_STATUS = 0,
    DYN_KPI,
    DYN_SPARK,
    DYN_SERVER,
    DYN_LIST,
    DYN_PET,
    DYN_MINI,
    DYN_UNKNOWN,
};

typedef struct {
    char key[16];
    char id[DASH_METRIC_ID_LEN];
    char label[48];
    char unit[16];
    char mtype[12];
    int precision;
    dash_metric_t *metric;
} dyn_binding_t;

typedef struct {
    char label[32];
    char binding[DASH_METRIC_ID_LEN];
    char kind[12];
    char format[24];
    char unit[16];
    char mtype[12];
    int precision;
    dash_metric_t *metric;
} dyn_row_t;

typedef struct {
    int type;
    int x, y, w, h;
    int body_w, body_h;
    bool show_title;
    char title[40];

    dyn_binding_t bindings[DYN_MAX_BINDINGS];
    int binding_count;

    dyn_row_t rows[DYN_MAX_ROWS];
    int row_count;
    bool list_show_labels;
    int list_font_size;
    int list_row_height;
    int list_label_width;

    char value_unit[16];
    int value_precision;
    char value_label[48];
    char value_type[12];
    dash_metric_t *value_metric;

    dash_metric_t *online_metric;
    dash_metric_t *model_metric;
    dash_metric_t *phase_metric;

    struct {
        char key[16];
        char label[48];
        char unit[16];
        char mtype[12];
        int precision;
        dash_metric_t *metric;
    } server[4];
    dash_metric_t *ram_total_metric;
    char ram_total_unit[16];
    char ram_total_mtype[12];
    int ram_total_precision;

    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *status_dot;
    lv_obj_t *status_model;
    lv_obj_t *status_phase;
    lv_obj_t *kpi_value;
    lv_obj_t *kpi_unit;
    lv_obj_t *kpi_gauge_arc;
    lv_obj_t *kpi_gauge_bar;
    lv_obj_t *skin_deco;
    lv_obj_t *chart;
    lv_chart_series_t *chart_series;
    float spark_hist[60];
    int spark_count;
    int spark_next;
    lv_obj_t *server_label[4];
    lv_obj_t *server_value[4];
    lv_obj_t *list_dot[DYN_MAX_ROWS];
    lv_obj_t *list_label[DYN_MAX_ROWS];
    lv_obj_t *list_value[DYN_MAX_ROWS];

    /* 桌宠 */
    dash_metric_t *pet_busy_metric;
    int pet_sleep_below;
    int pet_idle_below;
    int pet_happy_below;
    ui_pet_t pet;

    /* 小组件 2.0（ui_mini 渲染） */
    char mini_type[24];
    ui_mini_spec_t mini_spec;
    ui_mini_t *mini;
} dyn_widget_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *deco;
    dyn_widget_t *widgets;
    int widget_count;
    /* 传统框架：非 NULL 时整屏由 ui_classic 渲染，widgets 为空 */
    ui_classic_t *classic;
} dyn_view_t;

static dyn_view_t *s_view;
static dyn_view_t *s_staging;
static lv_obj_t *s_pending_blank;
static bool s_engaged;
static lv_obj_t *s_portal_overlay;
static lv_obj_t *s_network_overlay;
static lv_obj_t *s_net_state;
static lv_obj_t *s_net_ip;
static lv_obj_t *s_net_ssid;
static lv_obj_t *s_net_name;
static lv_obj_t *s_net_id;
static lv_obj_t *s_net_hint;
static lv_obj_t *s_portal_ssid;
static lv_obj_t *s_portal_pass;
static lv_obj_t *s_portal_url;

static uint32_t s_apply_count;
static int64_t s_apply_sum_us;
static int64_t s_apply_last_us;
static int64_t s_apply_max_us;
static lv_timer_t *s_pet_timer;
static lv_timer_t *s_deco_timer;
static uint32_t s_deco_phase;

/* ---------------- JSON helpers ---------------- */

static const char *jstr(cJSON *obj, const char *key, const char *def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return def;
}

static int jint(cJSON *obj, const char *key, int def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return def;
}

static double jdbl(cJSON *obj, const char *key, double def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return def;
}

static bool jbool(cJSON *obj, const char *key, bool def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return def;
}

/* 服务端 resolved 布局会带 canvas；旧服务端只有 profile 字符串，按已知方向回退。 */
static bool canvas_from_json(cJSON *root, dyn_canvas_t *out)
{
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "canvas");
    if (!cJSON_IsObject(node)) return false;
    int w = jint(node, "width", 0);
    int h = jint(node, "height", 0);
    int cols = jint(node, "cols", 0);
    int rows = jint(node, "rows", 0);
    if (w <= 0 || h <= 0 || cols <= 0 || rows <= 0) return false;
    out->w = w;
    out->h = h;
    out->cols = cols;
    out->rows = rows;
    out->gap = jint(node, "gap", 6);
    out->pad = jint(node, "padding", 8);
    return true;
}

static bool canvas_from_profile(const char *name, dyn_canvas_t *out)
{
    if (!name) return false;
    if (strcmp(name, "320x480") == 0) {
        *out = (dyn_canvas_t){320, 480, 12, 18, 8, 4};
        return true;
    }
    if (strcmp(name, "480x320") == 0) {
        *out = (dyn_canvas_t){480, 320, 18, 12, 8, 4};
        return true;
    }
    return false;
}

static uint32_t parse_hex_color(const char *s, uint32_t def)
{
    if (!s || s[0] != '#') return def;
    char *end = NULL;
    unsigned long value = strtoul(s + 1, &end, 16);
    if (end == s + 1) return def;
    return (uint32_t)value & 0xFFFFFFu;
}

static uint32_t jhex(cJSON *obj, const char *key, uint32_t def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return def;
    return parse_hex_color(item->valuestring, def);
}

/** 主题配色由服务端注入布局：theme_tokens。缺字段时回退经典配色。 */
static ui_skin_t skin_from_name(const char *name)
{
    if (!name) return UI_SKIN_NEON;
    if (strcmp(name, "minimal-air") == 0) return UI_SKIN_MINIMAL;
    if (strcmp(name, "vintage-gauge") == 0) return UI_SKIN_VINTAGE;
    if (strcmp(name, "vital-pulse") == 0) return UI_SKIN_VITAL;
    return UI_SKIN_NEON;
}

static ui_pet_style_t pet_style_from_name(const char *name)
{
    if (!name) return UI_PET_CYBER;
    if (strcmp(name, "blob") == 0) return UI_PET_BLOB;
    if (strcmp(name, "brass-cat") == 0) return UI_PET_BRASS;
    if (strcmp(name, "pulse-buddy") == 0) return UI_PET_PULSE;
    return UI_PET_CYBER;
}

static void parse_theme_tokens(cJSON *root)
{
    ui_theme_t theme;
    ui_theme_default(&theme);
    cJSON *tokens = cJSON_GetObjectItemCaseSensitive(root, "theme_tokens");
    if (cJSON_IsObject(tokens)) {
        theme.bg = jhex(tokens, "bg", theme.bg);
        theme.surface = jhex(tokens, "surface", theme.surface);
        theme.surface_2 = jhex(tokens, "surface_2", theme.surface_2);
        theme.border = jhex(tokens, "border", theme.border);
        theme.text = jhex(tokens, "text", theme.text);
        theme.text_dim = jhex(tokens, "text_dim", theme.text_dim);
        theme.text_mut = jhex(tokens, "text_mut", theme.text_mut);
        theme.accent = jhex(tokens, "accent", theme.accent);
        theme.accent_2 = jhex(tokens, "accent_2", theme.accent_2);
        theme.cyan = jhex(tokens, "cyan", theme.cyan);
        theme.ok = jhex(tokens, "ok", theme.ok);
        theme.warn = jhex(tokens, "warn", theme.warn);
        theme.err = jhex(tokens, "err", theme.err);
        theme.pink = jhex(tokens, "pink", theme.pink);
        theme.radius = jint(tokens, "radius", theme.radius);
        theme.skin = skin_from_name(jstr(tokens, "skin", "neon-cyber"));
        theme.surface_grad_from =
            jhex(tokens, "surface_grad_from", theme.surface_grad_from);
        theme.surface_grad_to =
            jhex(tokens, "surface_grad_to", theme.surface_grad_to);
        theme.glow = jhex(tokens, "glow", theme.glow);
        theme.glow_opa = jint(tokens, "glow_opa", theme.glow_opa);
        theme.border_width = jint(tokens, "border_width", theme.border_width);
        theme.corner_len = jint(tokens, "corner_len", theme.corner_len);
        theme.font_style = (strcmp(jstr(tokens, "font_style", "sans"), "mono") == 0)
                               ? UI_FONT_MONO
                               : UI_FONT_SANS;
        theme.tracking = jint(tokens, "tracking", theme.tracking);
        theme.pet_style =
            pet_style_from_name(jstr(tokens, "pet_variant", "cyber-cat"));
        const char *deco = jstr(tokens, "deco", "corners");
        theme.deco = (strcmp(deco, "corners") == 0) ? UI_DECO_CORNERS
                     : (strcmp(deco, "ticks") == 0) ? UI_DECO_TICKS
                     : (strcmp(deco, "pulse") == 0) ? UI_DECO_PULSE
                                                    : UI_DECO_NONE;
    }
    const char *id = jstr(root, "theme", "neon-cyber");
    ESP_LOGI(TAG, "theme '%s' skin=%d bg=#%06X accent=#%06X radius=%d",
             id, (int)theme.skin, (unsigned)theme.bg, (unsigned)theme.accent,
             theme.radius);
    ui_theme_set(&theme);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    size_t len = strlen(tmp);
    if (len >= cap) len = cap - 1;
    memmove(dst, tmp, len);
    dst[len] = '\0';
}

static int type_from_name(const char *name)
{
    if (!name) return DYN_UNKNOWN;
    if (ui_mini_is_type(name)) return DYN_MINI;
    if (strcmp(name, "status_pill") == 0) return DYN_STATUS;
    if (strcmp(name, "kpi") == 0) return DYN_KPI;
    if (strcmp(name, "sparkline") == 0) return DYN_SPARK;
    if (strcmp(name, "server_card") == 0) return DYN_SERVER;
    if (strcmp(name, "metric_list") == 0) return DYN_LIST;
    if (strcmp(name, "desktop_pet") == 0) return DYN_PET;
    return DYN_UNKNOWN;
}

static const char *type_label(int type)
{
    switch (type) {
    case DYN_STATUS: return "状态条";
    case DYN_KPI: return "大数字";
    case DYN_SPARK: return "迷你曲线";
    case DYN_SERVER: return "服务器";
    case DYN_LIST: return "指标列表";
    case DYN_PET: return "桌宠";
    case DYN_MINI: return "小组件";
    default: return "组件";
    }
}

static dyn_binding_t *find_binding(dyn_widget_t *w, const char *key)
{
    for (int i = 0; i < w->binding_count; i++) {
        if (strcmp(w->bindings[i].key, key) == 0) return &w->bindings[i];
    }
    return NULL;
}

/* ---------------- layout parsing ---------------- */

static void add_binding(dyn_widget_t *w, const char *key, const char *id,
                        cJSON *labels, cJSON *units, cJSON *precisions,
                        cJSON *types)
{
    if (!key || !id || !id[0] || w->binding_count >= DYN_MAX_BINDINGS) return;
    dyn_binding_t *b = &w->bindings[w->binding_count++];
    memset(b, 0, sizeof(*b));
    copy_str(b->key, sizeof(b->key), key);
    copy_str(b->id, sizeof(b->id), id);
    copy_str(b->label, sizeof(b->label), jstr(labels, key, key));
    copy_str(b->unit, sizeof(b->unit), jstr(units, key, ""));
    b->precision = jint(precisions, key, 0);
    copy_str(b->mtype, sizeof(b->mtype), jstr(types, key, ""));
    b->metric = dash_metric_store_get(id);
}

static bool parse_widget(cJSON *wj, dyn_widget_t *w)
{
    memset(w, 0, sizeof(*w));
    const char *type_name = jstr(wj, "type", "");
    w->type = type_from_name(type_name);
    if (w->type == DYN_UNKNOWN) return false;
    if (w->type == DYN_MINI) {
        copy_str(w->mini_type, sizeof(w->mini_type), type_name);
    }

    w->x = jint(wj, "x", 0);
    w->y = jint(wj, "y", 0);
    w->w = jint(wj, "w", 6);
    w->h = jint(wj, "h", 3);
    w->show_title = true;
    w->list_show_labels = true;
    w->list_font_size = 1; /* M */
    w->pet_sleep_below = 20;
    w->pet_idle_below = 60;
    w->pet_happy_below = 85;
    w->pet.state = -1;

    cJSON *options = cJSON_GetObjectItemCaseSensitive(wj, "options");
    cJSON *labels = NULL, *units = NULL, *precisions = NULL, *types = NULL;
    const char *title_def = (w->type == DYN_MINI)
                                ? ui_mini_label(w->mini_type)
                                : type_label(w->type);
    if (cJSON_IsObject(options)) {
        w->show_title = jbool(options, "show_title", true);
        copy_str(w->title, sizeof(w->title), jstr(options, "title", title_def));
        labels = cJSON_GetObjectItemCaseSensitive(options, "labels");
        units = cJSON_GetObjectItemCaseSensitive(options, "units");
        precisions = cJSON_GetObjectItemCaseSensitive(options, "precisions");
        types = cJSON_GetObjectItemCaseSensitive(options, "types");
        w->list_show_labels = jbool(options, "show_labels", true);
        const char *fs = jstr(options, "font_size", "M");
        w->list_font_size = (fs[0] == 'S') ? 0 : (fs[0] == 'L') ? 2 : 1;
        w->list_row_height = jint(options, "row_height", 0);
        w->list_label_width = jint(options, "label_width", 0);
    } else {
        copy_str(w->title, sizeof(w->title), title_def);
    }

    /* 小组件 2.0 规格 */
    if (w->type == DYN_MINI) {
        ui_mini_spec_t *sp = &w->mini_spec;
        memset(sp, 0, sizeof(*sp));
        snprintf(sp->type, sizeof(sp->type), "%s", w->mini_type);
        copy_str(sp->title, sizeof(sp->title), w->title);
        if (cJSON_IsObject(options)) {
            copy_str(sp->unit, sizeof(sp->unit), jstr(options, "unit", ""));
            copy_str(sp->unit2, sizeof(sp->unit2), jstr(options, "unit2", ""));
            copy_str(sp->on_text, sizeof(sp->on_text),
                     jstr(options, "on_text", "在线"));
            copy_str(sp->off_text, sizeof(sp->off_text),
                     jstr(options, "off_text", "离线"));
            sp->max_value = jdbl(options, "max_value", 0);
            sp->warn_above = jdbl(options, "warn_above", 0);
            sp->danger_above = jdbl(options, "danger_above", 0);
            sp->show_ticks = jbool(options, "show_ticks", true);
            sp->show_labels = jbool(options, "show_labels", true);
            sp->show_values = jbool(options, "show_values", true);
            sp->show_trend = jbool(options, "show_trend", true);
            sp->show_area = jbool(options, "show_area", true);
            const char *mfs = jstr(options, "font_size", "M");
            sp->font_size = (mfs[0] == 'S') ? 0 : (mfs[0] == 'L') ? 2 : 1;
            const char *al = jstr(options, "align", "left");
            sp->align = (al[0] == 'c') ? 1 : 0;
        }
    }

    cJSON *bindings = cJSON_GetObjectItemCaseSensitive(wj, "bindings");
    if (cJSON_IsObject(bindings)) {
        for (cJSON *c = bindings->child; c; c = c->next) {
            if (c->string && cJSON_IsString(c)) {
                add_binding(w, c->string, c->valuestring, labels, units,
                            precisions, types);
            }
        }
    }

    if (w->type == DYN_KPI || w->type == DYN_SPARK || w->type == DYN_PET) {
        dyn_binding_t *b = find_binding(w, "value");
        if (!b) return false;
        w->value_metric = b->metric;
        copy_str(w->value_unit, sizeof(w->value_unit), b->unit);
        w->value_precision = b->precision;
        copy_str(w->value_label, sizeof(w->value_label), b->label);
        copy_str(w->value_type, sizeof(w->value_type), b->mtype);
        if (cJSON_IsObject(options)) {
            const char *unit = jstr(options, "unit", NULL);
            if (unit) copy_str(w->value_unit, sizeof(w->value_unit), unit);
            w->value_precision = jint(options, "precision", w->value_precision);
            const char *ml = jstr(options, "metric_label", NULL);
            if (ml) copy_str(w->value_label, sizeof(w->value_label), ml);
            const char *mt = jstr(options, "metric_type", NULL);
            if (mt) copy_str(w->value_type, sizeof(w->value_type), mt);
        }
        if (w->type == DYN_PET) {
            if (cJSON_IsObject(options)) {
                w->pet_sleep_below = jint(options, "sleep_below", w->pet_sleep_below);
                w->pet_idle_below = jint(options, "idle_below", w->pet_idle_below);
                w->pet_happy_below = jint(options, "happy_below", w->pet_happy_below);
            }
            dyn_binding_t *busy = find_binding(w, "busy");
            if (busy) w->pet_busy_metric = busy->metric;
        }
    } else if (w->type == DYN_STATUS) {
        dyn_binding_t *b;
        b = find_binding(w, "online");
        if (b) w->online_metric = b->metric;
        b = find_binding(w, "model");
        if (b) w->model_metric = b->metric;
        b = find_binding(w, "phase");
        if (b) w->phase_metric = b->metric;
    } else if (w->type == DYN_SERVER) {
        static const char *keys[4] = {"cpu", "ram_used", "gpu_temp", "ip"};
        static const char *labels_fallback[4] = {"CPU", "内存", "GPU", "IP"};
        for (int i = 0; i < 4; i++) {
            copy_str(w->server[i].key, sizeof(w->server[i].key), keys[i]);
            copy_str(w->server[i].label, sizeof(w->server[i].label),
                     labels_fallback[i]);
            dyn_binding_t *b = find_binding(w, keys[i]);
            if (b) {
                w->server[i].metric = b->metric;
                copy_str(w->server[i].label, sizeof(w->server[i].label), b->label);
                copy_str(w->server[i].unit, sizeof(w->server[i].unit), b->unit);
                copy_str(w->server[i].mtype, sizeof(w->server[i].mtype), b->mtype);
                w->server[i].precision = b->precision;
            }
        }
        dyn_binding_t *rt = find_binding(w, "ram_total");
        if (rt) {
            w->ram_total_metric = rt->metric;
            copy_str(w->ram_total_unit, sizeof(w->ram_total_unit), rt->unit);
            copy_str(w->ram_total_mtype, sizeof(w->ram_total_mtype), rt->mtype);
            w->ram_total_precision = rt->precision;
        }
    }

    /* 行式组件通用解析：metric_list / led_matrix / multi_bars / spark_row */
    {
        cJSON *rows = cJSON_IsObject(options)
                          ? cJSON_GetObjectItemCaseSensitive(options, "rows")
                          : NULL;
        if (cJSON_IsArray(rows)) {
            int n = cJSON_GetArraySize(rows);
            for (int i = 0; i < n && w->row_count < DYN_MAX_ROWS; i++) {
                cJSON *rj = cJSON_GetArrayItem(rows, i);
                if (!cJSON_IsObject(rj)) continue;
                dyn_row_t *row = &w->rows[w->row_count++];
                memset(row, 0, sizeof(*row));
                copy_str(row->label, sizeof(row->label), jstr(rj, "label", ""));
                copy_str(row->binding, sizeof(row->binding),
                         jstr(rj, "binding", ""));
                copy_str(row->kind, sizeof(row->kind), jstr(rj, "kind", "auto"));
                copy_str(row->format, sizeof(row->format), jstr(rj, "format", ""));
                copy_str(row->unit, sizeof(row->unit), jstr(rj, "unit", ""));
                row->precision = jint(rj, "precision", 0);
                copy_str(row->mtype, sizeof(row->mtype),
                         jstr(rj, "metric_type", ""));
                row->metric = dash_metric_store_get(row->binding);
                if (!row->label[0]) {
                    copy_str(row->label, sizeof(row->label), row->binding);
                }
            }
        }
    }

    return true;
}

static void collect_id(char ids[][DASH_METRIC_ID_LEN], int *count, const char *id)
{
    if (!id || !id[0] || *count >= DYN_MAX_SUB_IDS) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(ids[i], id) == 0) return;
    }
    copy_str(ids[*count], DASH_METRIC_ID_LEN, id);
    (*count)++;
}

static void collect_widget_ids(cJSON *wj, char ids[][DASH_METRIC_ID_LEN], int *count)
{
    cJSON *bindings = cJSON_GetObjectItemCaseSensitive(wj, "bindings");
    if (cJSON_IsObject(bindings)) {
        for (cJSON *c = bindings->child; c; c = c->next) {
            if (cJSON_IsString(c)) collect_id(ids, count, c->valuestring);
        }
    }
    cJSON *options = cJSON_GetObjectItemCaseSensitive(wj, "options");
    cJSON *rows = cJSON_IsObject(options)
                      ? cJSON_GetObjectItemCaseSensitive(options, "rows")
                      : NULL;
    if (cJSON_IsArray(rows)) {
        int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; i++) {
            cJSON *rj = cJSON_GetArrayItem(rows, i);
            if (cJSON_IsObject(rj)) {
                collect_id(ids, count, jstr(rj, "binding", NULL));
            }
        }
    }
}

static void build_subscribe(char ids[][DASH_METRIC_ID_LEN], int count,
                            char *out, size_t cap)
{
    if (!out || cap < 32) return;
    if (count == 0) {
        snprintf(out, cap, "{\"type\":\"subscribe\"}");
        return;
    }
    size_t used = (size_t)snprintf(out, cap, "{\"type\":\"subscribe\",\"metrics\":[");
    for (int i = 0; i < count && used + 48 < cap; i++) {
        used += (size_t)snprintf(out + used, cap - used, "%s\"%s\"",
                                 i ? "," : "", ids[i]);
    }
    snprintf(out + used, cap - used, "]}");
}

/* ---------------- LVGL build ---------------- */

static void grid_rect(int gx, int gy, int gw, int gh,
                      int *ox, int *oy, int *ow, int *oh)
{
    const float cw = (s_canvas.w - 2.0f * s_canvas.pad -
                      (s_canvas.cols - 1) * s_canvas.gap) / s_canvas.cols;
    const float ch = (s_canvas.h - 2.0f * s_canvas.pad -
                      (s_canvas.rows - 1) * s_canvas.gap) / s_canvas.rows;
    *ox = (int)lroundf(s_canvas.pad + gx * (cw + s_canvas.gap));
    *oy = (int)lroundf(s_canvas.pad + gy * (ch + s_canvas.gap));
    *ow = (int)lroundf(gw * cw + (gw - 1) * s_canvas.gap);
    *oh = (int)lroundf(gh * ch + (gh - 1) * s_canvas.gap);
}

/* 运行时中文字体：服务端按布局文字下发的三档 binfont（11/13/15）；
 * 失败时回退内置 16px 子集 */
static const lv_font_t *s_cjk_small;
static const lv_font_t *s_cjk_medium;
static const lv_font_t *s_cjk_large;

static const lv_font_t *cjk_small(void)
{
    return s_cjk_small ? s_cjk_small : UI_F_CJK;
}

static const lv_font_t *cjk_medium(void)
{
    return s_cjk_medium ? s_cjk_medium : UI_F_CJK;
}

static const lv_font_t *cjk_large(void)
{
    return s_cjk_large ? s_cjk_large : UI_F_CJK;
}

void ui_dynamic_set_cjk_fonts(const lv_font_t *small,
                              const lv_font_t *medium,
                              const lv_font_t *large)
{
    s_cjk_small = small;
    s_cjk_medium = medium;
    s_cjk_large = large;
}

const lv_font_t *ui_dynamic_font_small(void) { return cjk_small(); }
const lv_font_t *ui_dynamic_font_medium(void) { return cjk_medium(); }
const lv_font_t *ui_dynamic_font_large(void) { return cjk_large(); }

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/** 纯 ASCII 文案用 Montserrat，避免中文字库的拉丁字形发虚/挤在一起 */
static bool text_is_ascii(const char *text)
{
    for (; text && *text; text++) {
        if ((unsigned char)*text >= 0x80) return false;
    }
    return true;
}

static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *row = ui_group_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *make_dot(lv_obj_t *parent, int size)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(
        dot, th->skin == UI_SKIN_VINTAGE ? 1 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ui_theme()->text_mut), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    if (th->skin == UI_SKIN_VITAL) {
        lv_obj_set_style_outline_width(dot, 1, 0);
        lv_obj_set_style_outline_color(dot, lv_color_hex(th->accent_2), 0);
        lv_obj_set_style_outline_opa(dot, LV_OPA_60, 0);
        lv_obj_set_style_outline_pad(dot, 2, 0);
    }
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static void draw_seg(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                     lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

/* 每个组件最多在绘制阶段补几条线，不新增 LVGL 对象，避免内部 RAM 紧张。 */
static void card_deco_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    int code = (int)(uintptr_t)lv_event_get_user_data(e);
    int skin = code >> 8;
    int type = code & 0xFF;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const ui_theme_t *th = ui_theme();
    lv_color_t accent = lv_color_hex(th->accent);
    lv_color_t second = lv_color_hex(th->accent_2);

    if (skin == UI_SKIN_NEON) {
        const int len = 8;
        const int inset = 3;
        int x1 = a.x1 + inset, y1 = a.y1 + inset;
        int x2 = a.x2 - inset, y2 = a.y2 - inset;
        draw_seg(layer, x1, y1, x1 + len, y1, accent, 1, LV_OPA_70);
        draw_seg(layer, x1, y1, x1, y1 + len, accent, 1, LV_OPA_70);
        draw_seg(layer, x2, y1, x2 - len, y1, accent, 1, LV_OPA_70);
        draw_seg(layer, x2, y1, x2, y1 + len, accent, 1, LV_OPA_70);
        draw_seg(layer, x1, y2, x1 + len, y2, accent, 1, LV_OPA_70);
        draw_seg(layer, x1, y2, x1, y2 - len, accent, 1, LV_OPA_70);
        draw_seg(layer, x2, y2, x2 - len, y2, accent, 1, LV_OPA_70);
        draw_seg(layer, x2, y2, x2, y2 - len, accent, 1, LV_OPA_70);
        for (int i = 1; i <= 2; i++) {
            int y = a.y1 + (a.y2 - a.y1) * i / 3;
            draw_seg(layer, a.x1 + 5, y, a.x2 - 5, y, accent, 1, LV_OPA_10);
        }
    } else if (skin == UI_SKIN_VINTAGE) {
        const int inset = 5;
        int dot = 3;
        int xs[2] = {a.x1 + inset, a.x2 - inset};
        int ys[2] = {a.y1 + inset, a.y2 - inset};
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                draw_seg(layer, xs[i], ys[j], xs[i], ys[j], second, dot,
                         LV_OPA_70);
            }
        }
        int ticks = 8;
        int span = a.x2 - a.x1 - 16;
        int ybase = a.y2 - 5;
        for (int i = 0; i < ticks; i++) {
            int x = a.x1 + 8 + span * i / (ticks - 1);
            draw_seg(layer, x, ybase, x, ybase - 3, second, 1, LV_OPA_50);
        }
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP;
        rd.border_width = 1;
        rd.border_color = second;
        rd.border_opa = LV_OPA_20;
        rd.radius = 6;
        lv_area_t inner = {a.x1 + 3, a.y1 + 3, a.x2 - 3, a.y2 - 3};
        lv_draw_rect(layer, &rd, &inner);
    } else if (skin == UI_SKIN_VITAL && (type == DYN_KPI || type == DYN_SPARK)) {
        int x1 = a.x1 + 7;
        int x2 = a.x2 - 7;
        int y = a.y2 - 6;
        int mid = (x1 + x2) / 2;
        lv_point_precise_t pts[6] = {
            {x1, y}, {mid - 12, y}, {mid - 7, y - 4},
            {mid - 2, y + 4}, {mid + 4, y}, {x2, y},
        };
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = accent;
        d.width = 1;
        d.opa = LV_OPA_50;
#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 4)
        d.points = pts;
        d.point_cnt = 6;
        lv_draw_line(layer, &d);
#else
        for (int i = 1; i < 6; i++) {
            draw_seg(layer, (int)pts[i - 1].x, (int)pts[i - 1].y, (int)pts[i].x,
                     (int)pts[i].y, accent, 1, LV_OPA_50);
        }
#endif
    }
}

static void screen_deco_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const ui_theme_t *th = ui_theme();
    lv_color_t accent = lv_color_hex(th->accent);
    lv_color_t second = lv_color_hex(th->accent_2);

    if (th->skin == UI_SKIN_NEON) {
        int cell = (s_canvas.w - 2 * s_canvas.pad -
                    (s_canvas.cols - 1) * s_canvas.gap) / s_canvas.cols;
        int step = cell + s_canvas.gap;
        if (step < 8) step = 8;
        for (int y = a.y1; y <= a.y2; y += step) {
            draw_seg(layer, a.x1, y, a.x2, y, accent, 1, LV_OPA_10);
        }
        for (int x = a.x1; x <= a.x2; x += step) {
            draw_seg(layer, x, a.y1, x, a.y2, accent, 1, LV_OPA_10);
        }
    } else if (th->skin == UI_SKIN_VINTAGE) {
        int inset = 6;
        draw_seg(layer, a.x1 + inset, a.y1 + inset, a.x2 - inset,
                 a.y1 + inset, second, 1, LV_OPA_30);
        draw_seg(layer, a.x1 + inset, a.y2 - inset, a.x2 - inset,
                 a.y2 - inset, second, 1, LV_OPA_30);
        draw_seg(layer, a.x1 + inset, a.y1 + inset, a.x1 + inset,
                 a.y2 - inset, second, 1, LV_OPA_30);
        draw_seg(layer, a.x2 - inset, a.y1 + inset, a.x2 - inset,
                 a.y2 - inset, second, 1, LV_OPA_30);
    } else if (th->skin == UI_SKIN_VITAL) {
        const int radii[3] = {70, 110, 150};
        int cx = a.x1 + (a.x2 - a.x1) * 3 / 4;
        int cy = a.y1 + (a.y2 - a.y1) * 4 / 5;
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.color = second;
        ad.width = 1;
        ad.opa = LV_OPA_10;
        ad.center.x = cx;
        ad.center.y = cy;
        ad.start_angle = 0;
        ad.end_angle = 360;
        for (int i = 0; i < 3; i++) {
            ad.radius = radii[i];
            lv_draw_arc(layer, &ad);
        }
    }
}

static void draw_ring(lv_layer_t *layer, int cx, int cy, int radius,
                      lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = radius;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(layer, &d);
}

static void draw_gear(lv_layer_t *layer, int cx, int cy, int r_out, int r_in,
                      int teeth, float angle, lv_color_t color, lv_opa_t opa)
{
    const float step = 6.2831853f / teeth;
    for (int i = 0; i < teeth; i++) {
        float a = angle + step * i;
        int x1 = cx + (int)(cosf(a) * r_in);
        int y1 = cy + (int)(sinf(a) * r_in);
        int x2 = cx + (int)(cosf(a) * r_out);
        int y2 = cy + (int)(sinf(a) * r_out);
        draw_seg(layer, x1, y1, x2, y2, color, 3, opa);
    }
    draw_ring(layer, cx, cy, (r_out + r_in) / 2, color, r_out - r_in, LV_OPA_20);
    draw_ring(layer, cx, cy, r_in - 1, color, 1, opa);
    draw_ring(layer, cx, cy, 3, color, 3, opa);
}

/* 右下角的主题动态装饰：只失效自身 60×60 区域，不整屏重绘。 */
static void deco_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int cx = (a.x1 + a.x2) / 2;
    int cy = (a.y1 + a.y2) / 2;
    const ui_theme_t *th = ui_theme();
    lv_color_t accent = lv_color_hex(th->accent);
    lv_color_t second = lv_color_hex(th->accent_2);
    float angle = (float)(s_deco_phase % 3600) * 0.1f;

    if (th->skin == UI_SKIN_VINTAGE) {
        draw_gear(layer, cx - 5, cy, 15, 10, 12, angle * 0.0174533f,
                  accent, LV_OPA_40);
        draw_gear(layer, cx + 11, cy + 8, 9, 6, 9,
                  -angle * 0.0174533f * 1.6f, second, LV_OPA_30);
    } else if (th->skin == UI_SKIN_NEON) {
        draw_ring(layer, cx, cy, 18, accent, 1, LV_OPA_20);
        draw_ring(layer, cx, cy, 12, accent, 1, LV_OPA_10);
        float rad = angle * 0.0174533f;
        int x2 = cx + (int)(cosf(rad) * 18);
        int y2 = cy + (int)(sinf(rad) * 18);
        draw_seg(layer, cx, cy, x2, y2, accent, 2, LV_OPA_50);
        draw_seg(layer, cx, cy, cx, cy, accent, 4, LV_OPA_60);
    } else if (th->skin == UI_SKIN_VITAL) {
        float pulse = fabsf(sinf((float)s_deco_phase * 0.06f));
        lv_opa_t opa = (lv_opa_t)(20 + 35 * pulse);
        draw_ring(layer, cx, cy, 17, accent, 2, opa);
        draw_ring(layer, cx, cy, 11, second, 1, LV_OPA_20);
        int y = cy;
        lv_point_precise_t pts[6] = {
            {cx - 14, y}, {cx - 7, y}, {cx - 3, y - 3},
            {cx + 1, y + 3}, {cx + 5, y}, {cx + 14, y},
        };
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = second;
        d.width = 1;
        d.opa = LV_OPA_60;
#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 4)
        d.points = pts;
        d.point_cnt = 6;
        lv_draw_line(layer, &d);
#else
        for (int i = 1; i < 6; i++) {
            draw_seg(layer, (int)pts[i - 1].x, (int)pts[i - 1].y, (int)pts[i].x,
                     (int)pts[i].y, second, 1, LV_OPA_60);
        }
#endif
    } else {
        float pulse = fabsf(sinf((float)s_deco_phase * 0.04f));
        draw_seg(layer, cx, cy, cx, cy, second, 3,
                 (lv_opa_t)(40 + 60 * pulse));
    }
}

static void deco_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_view || !s_view->deco) return;
    s_deco_phase += 4;
    lv_obj_invalidate(s_view->deco);
}

/* 装饰不再浮在屏幕角落，而是作为服务器卡右下角的水印，随卡片一起布局。 */
static void deco_create(dyn_view_t *v)
{
    if (!v || !v->screen) return;
    if (ui_theme()->skin == UI_SKIN_MINIMAL) return;
    lv_obj_t *host = NULL;
    for (int i = 0; i < v->widget_count; i++) {
        if (v->widgets[i].type == DYN_SERVER && v->widgets[i].root &&
            v->widgets[i].w >= 8) {
            host = v->widgets[i].root;
            break;
        }
    }
    if (!host) return;
    lv_obj_t *deco = lv_obj_create(host);
    lv_obj_remove_style_all(deco);
    lv_obj_set_size(deco, 44, 44);
    lv_obj_align(deco, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(deco, deco_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_move_background(deco);
    v->deco = deco;
}

static void deco_timer_sync(void)
{
    bool any = s_view && s_view->deco;
    if (any && !s_deco_timer) {
        s_deco_timer = lv_timer_create(deco_timer_cb, 60, NULL);
    } else if (!any && s_deco_timer) {
        lv_timer_delete(s_deco_timer);
        s_deco_timer = NULL;
    }
}

static void style_card_skin(lv_obj_t *card)
{
    const ui_theme_t *th = ui_theme();
    int bw = th->border_width > 0 ? th->border_width : 1;
    lv_obj_set_style_border_width(card, bw, 0);
    if (th->skin != UI_SKIN_MINIMAL) {
        lv_obj_set_style_bg_grad_color(card, lv_color_hex(th->surface_grad_to), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    }
    if (th->skin == UI_SKIN_NEON) {
        lv_obj_set_style_outline_width(card, 1, 0);
        lv_obj_set_style_outline_color(card, lv_color_hex(th->glow), 0);
        lv_obj_set_style_outline_opa(card, (lv_opa_t)(th->glow_opa * 255 / 400), 0);
    } else if (th->skin == UI_SKIN_VINTAGE) {
        lv_obj_set_style_outline_width(card, 1, 0);
        lv_obj_set_style_outline_color(card, lv_color_hex(th->accent_2), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_20, 0);
        lv_obj_set_style_outline_pad(card, -3, 0);
    } else if (th->skin == UI_SKIN_VITAL) {
        lv_obj_set_style_outline_width(card, 1, 0);
        lv_obj_set_style_outline_color(card, lv_color_hex(th->accent), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_20, 0);
    }
}

static void style_title_skin(lv_obj_t *title)
{
    const ui_theme_t *th = ui_theme();
    uint32_t color = th->text_dim;
    if (th->skin == UI_SKIN_NEON) color = th->accent;
    else if (th->skin == UI_SKIN_VINTAGE) color = th->accent_2;
    lv_obj_set_style_text_color(title, lv_color_hex(color), 0);
    if (th->tracking > 0) {
        lv_obj_set_style_text_letter_space(title, th->tracking, 0);
    }
}

static float metric_ratio(const dash_metric_t *m, const char *unit)
{
    if (!m || !m->available || m->type != DASH_METRIC_NUMBER) return 0.62f;
    float value = (float)m->number;
    if (unit && (strcmp(unit, "%") == 0 || strstr(unit, "°C") != NULL)) {
        value /= 100.0f;
    } else {
        value = value / (value + 1.0f);
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value;
}

static void style_row_separator(lv_obj_t *row, int index)
{
    const ui_theme_t *th = ui_theme();
    if (index <= 0) return;
    if (th->skin == UI_SKIN_NEON) return;
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(th->border), 0);
    lv_obj_set_style_border_opa(row, th->skin == UI_SKIN_VINTAGE ? LV_OPA_50
                                                                : LV_OPA_30, 0);
}

static void build_skin_deco(dyn_widget_t *w)
{
    const ui_theme_t *th = ui_theme();
    if (th->skin == UI_SKIN_MINIMAL || th->skin == UI_SKIN_VITAL) return;
    lv_obj_t *deco = lv_obj_create(w->root);
    lv_obj_remove_style_all(deco);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_CLICKABLE);
    if (th->skin == UI_SKIN_VITAL) {
        lv_obj_set_size(deco, 36, 36);
        lv_obj_align(deco, LV_ALIGN_TOP_RIGHT, 8, -8);
        lv_obj_set_style_radius(deco, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(deco, lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_opa(deco, LV_OPA_10, 0);
    } else if (th->skin == UI_SKIN_NEON) {
        lv_obj_set_size(deco, LV_PCT(100), 1);
        lv_obj_align(deco, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(deco, lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_opa(deco, LV_OPA_30, 0);
    } else {
        lv_obj_set_size(deco, LV_PCT(100), 1);
        lv_obj_align(deco, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(deco, lv_color_hex(th->accent_2), 0);
        lv_obj_set_style_bg_opa(deco, LV_OPA_20, 0);
    }
    lv_obj_move_background(deco);
    w->skin_deco = deco;
}

static void build_kpi_gauge(dyn_widget_t *w)
{
    const ui_theme_t *th = ui_theme();
    if (w->type != DYN_KPI) return;
    if (th->skin == UI_SKIN_VINTAGE || th->skin == UI_SKIN_VITAL) {
        int arc_size = th->skin == UI_SKIN_VINTAGE ? 52 : 48;
        lv_obj_t *arc = lv_arc_create(w->root);
        lv_obj_remove_style_all(arc);
        lv_obj_set_size(arc, arc_size, arc_size);
        lv_obj_align(arc, LV_ALIGN_TOP_RIGHT, -8, 8);
        lv_arc_set_rotation(arc, th->skin == UI_SKIN_VINTAGE ? 135 : 270);
        lv_arc_set_bg_angles(arc, 0, th->skin == UI_SKIN_VINTAGE ? 270 : 360);
        lv_arc_set_value(arc, 62);
        lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(th->border), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(th->accent),
                                   LV_PART_INDICATOR);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        w->kpi_gauge_arc = arc;
    } else {
        lv_obj_t *bar = lv_obj_create(w->root);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, w->body_w * 62 / 100, th->skin == UI_SKIN_NEON ? 4 : 2);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 6, -4);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_opa(bar, th->skin == UI_SKIN_NEON ? LV_OPA_50
                                                              : LV_OPA_80, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        w->kpi_gauge_bar = bar;
    }
}

static void build_status(dyn_widget_t *w)
{
    lv_obj_set_flex_flow(w->body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(w->body, 6, 0);

    w->status_dot = make_dot(w->body, 9);
    w->status_model = make_label(w->body, "—", cjk_medium(), ui_theme()->text);
    lv_obj_set_flex_grow(w->status_model, 1);
    lv_obj_set_style_max_width(w->status_model, LV_PCT(70), 0);
    lv_obj_set_height(w->status_model, 20);
    lv_label_set_long_mode(w->status_model, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *pill = ui_pill_create(w->body, "—", ui_theme()->accent);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_style_pad_hor(pill, 6, 0);
    w->status_phase = lv_obj_get_child(pill, 0);
}

static void build_kpi_like(dyn_widget_t *w)
{
    lv_obj_set_flex_flow(w->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w->body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(w->body, 1, 0);

    lv_obj_t *row = make_row(w->body);
    const ui_theme_t *th = ui_theme();
    const lv_font_t *value_font =
        (w->w <= 4) ? &lv_font_montserrat_24 : UI_F_H2;
    uint32_t value_color = th->text;
    if (th->skin == UI_SKIN_NEON) value_color = th->accent;
    w->kpi_value = make_label(row, "--", value_font, value_color);
    lv_label_set_long_mode(w->kpi_value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(
        w->kpi_value,
        LV_PCT((th->skin == UI_SKIN_VINTAGE || th->skin == UI_SKIN_VITAL) ? 60 : 72),
        0);
    if (th->skin == UI_SKIN_VINTAGE || th->skin == UI_SKIN_NEON) {
        lv_obj_set_style_text_letter_space(w->kpi_value, 1, 0);
    }
    w->kpi_unit = make_label(row, w->value_unit, &lv_font_montserrat_12, th->text_mut);

    if (!w->show_title) {
        lv_obj_t *label = make_label(w->body, w->value_label, cjk_small(),
                                     ui_theme()->text_mut);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    }

    if (w->type == DYN_SPARK) {
        int reserved = 38 + (w->show_title ? 0 : 20);
        int chart_h = w->body_h - reserved;
        if (chart_h < 24) chart_h = 24;
        if (chart_h > 70) chart_h = 70;
        w->chart = ui_chart_create(w->body, chart_h, ui_theme()->accent, 0,
                                   &w->chart_series, NULL);
        lv_chart_set_point_count(w->chart, 60);
        lv_chart_set_range(w->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1);
    }
}

static void build_server(dyn_widget_t *w)
{
    const int body_w = w->body_w;
    const int body_h = w->body_h;
    int row_h = body_h / 4;
    if (row_h < 18) row_h = 18;
    const int label_w = body_w * 45 / 100;
    const int value_w = body_w - label_w;
    const int label_y = (row_h - cjk_medium()->line_height) / 2;
    const int value_y = (row_h - (&lv_font_montserrat_12)->line_height) / 2;
    const ui_theme_t *th = ui_theme();
    uint32_t label_color = (th->skin == UI_SKIN_NEON) ? th->text_mut : th->text_dim;
    uint32_t value_color = th->text;
    if (th->skin == UI_SKIN_NEON) value_color = th->accent;
    else if (th->skin == UI_SKIN_VITAL) value_color = th->accent_2;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = ui_group_create(w->body);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, body_w, row_h);
        style_row_separator(row, i);

        w->server_label[i] = make_label(row, w->server[i].label, cjk_medium(),
                                        label_color);
        lv_obj_set_pos(w->server_label[i], 0, label_y > 0 ? label_y : 0);
        lv_obj_set_size(w->server_label[i], label_w,
                        cjk_medium()->line_height);
        lv_label_set_long_mode(w->server_label[i], LV_LABEL_LONG_MODE_DOTS);

        w->server_value[i] = make_label(row, "--", &lv_font_montserrat_12, value_color);
        lv_obj_set_pos(w->server_value[i], label_w, value_y > 0 ? value_y : 0);
        lv_obj_set_size(w->server_value[i], value_w,
                        (&lv_font_montserrat_12)->line_height);
        lv_obj_set_style_text_align(w->server_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(w->server_value[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_metric_list(dyn_widget_t *w)
{
    const int body_w = w->body_w;

    const lv_font_t *value_font =
        (w->list_font_size == 0) ? &lv_font_montserrat_12
        : (w->list_font_size == 2) ? &lv_font_montserrat_16
                                   : &lv_font_montserrat_14;
    const lv_font_t *label_font = (w->list_font_size == 0) ? cjk_small()
                                  : (w->list_font_size == 2) ? cjk_large()
                                                             : cjk_medium();

    /* 与 Web 编辑器一致：默认行高 22、标签宽 60，可在属性面板覆盖 */
    int row_h = w->list_row_height > 0 ? w->list_row_height : 22;
    int min_row_h = label_font->line_height + 1;
    if (row_h < min_row_h) row_h = min_row_h;
    int label_y = (row_h - label_font->line_height) / 2;
    if (label_y < 0) label_y = 0;
    const ui_theme_t *th = ui_theme();
    uint32_t label_color = (th->skin == UI_SKIN_NEON) ? th->text_mut : th->text_dim;
    uint32_t value_color = th->text;
    if (th->skin == UI_SKIN_NEON) value_color = th->accent;
    else if (th->skin == UI_SKIN_VITAL) value_color = th->accent_2;

    for (int i = 0; i < w->row_count; i++) {
        dyn_row_t *r = &w->rows[i];
        lv_obj_t *row = ui_group_create(w->body);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, body_w, row_h);
        style_row_separator(row, i);

        int x = 0;
        bool bool_status = (strcmp(r->mtype, "bool") == 0) ||
                           (strcmp(r->kind, "status") == 0);
        bool show_dot = bool_status;
        const lv_font_t *row_value_font = bool_status ? label_font : value_font;
        if (show_dot) {
            w->list_dot[i] = make_dot(row, 7);
            lv_obj_set_pos(w->list_dot[i], 0, (row_h - 7) / 2);
            x += 9;
        }
        int label_w = 0;
        if (w->list_show_labels) {
            label_w = w->list_label_width > 0 ? w->list_label_width : 60;
            if (label_w > body_w - x) label_w = body_w - x;
            w->list_label[i] = make_label(row, r->label, label_font, label_color);
            lv_obj_set_pos(w->list_label[i], x, label_y);
            lv_obj_set_size(w->list_label[i], label_w, label_font->line_height);
            lv_label_set_long_mode(w->list_label[i], LV_LABEL_LONG_MODE_DOTS);
            x += label_w;
        }
        int value_y = (row_h - row_value_font->line_height) / 2;
        if (value_y < 0) value_y = 0;
        w->list_value[i] = make_label(row, "--", row_value_font, value_color);
        lv_obj_set_pos(w->list_value[i], x, value_y);
        lv_obj_set_size(w->list_value[i], body_w - x,
                        row_value_font->line_height);
        lv_obj_set_style_text_align(w->list_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(w->list_value[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

static bool view_has_pet(dyn_view_t *v)
{
    if (!v) return false;
    for (int i = 0; i < v->widget_count; i++) {
        if (v->widgets[i].type == DYN_PET) return true;
    }
    return false;
}

static void pet_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_view) return;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < s_view->widget_count; i++) {
        dyn_widget_t *w = &s_view->widgets[i];
        if (w->type == DYN_PET) {
            ui_pet_tick(&w->pet, now);
        }
    }
}

static void pet_timer_sync(void)
{
    bool any = view_has_pet(s_view);
    if (any && !s_pet_timer) {
        s_pet_timer = lv_timer_create(pet_timer_cb, 40, NULL);
    } else if (!any && s_pet_timer) {
        lv_timer_delete(s_pet_timer);
        s_pet_timer = NULL;
    }
}

static void build_widget_ui(dyn_widget_t *w, lv_obj_t *parent)
{
    int x, y, ww, hh;
    grid_rect(w->x, w->y, w->w, w->h, &x, &y, &ww, &hh);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(ui_theme()->surface), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, ui_theme()->radius, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(ui_theme()->border), 0);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, ww, hh);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    style_card_skin(card);
    lv_obj_add_event_cb(
        card, card_deco_cb, LV_EVENT_DRAW_MAIN_END,
        (void *)(uintptr_t)(((int)ui_theme()->skin << 8) | (w->type & 0xFF)));
    w->root = card;

    int header_h = 0;
    /* 小组件在 60px 以下的卡片里不放标题栏：空间留给图形本身，
     * 标题由组件内部标签或 size 规格承担（与 Web 编辑器同规则）。 */
    bool mini_tiny = (w->type == DYN_MINI && hh < 60);
    if (w->show_title && !mini_tiny) {
        lv_obj_t *title = make_label(card, w->title, cjk_small(), ui_theme()->text_dim);
        lv_obj_set_pos(title, 8, 4);
        lv_obj_set_size(title, ww - 16, 18);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        style_title_skin(title);
        header_h = 22;
    }

    w->body = ui_group_create(card);
    int body_y = header_h ? header_h + 2 : 6;
    int body_h = hh - body_y - 6;
    if (body_h < 1) body_h = 1;
    /* 主题动态装饰画在宽服务器卡右下角，正文给它留出右侧空位。 */
    int deco_reserve = (w->type == DYN_SERVER &&
                        ui_theme()->skin != UI_SKIN_MINIMAL && ww >= 160)
                           ? 56
                           : 0;
    w->body_w = ww - 12 - deco_reserve;
    if (w->body_w < 1) w->body_w = 1;
    w->body_h = body_h;
    lv_obj_set_pos(w->body, 6, body_y);
    lv_obj_set_size(w->body, w->body_w, w->body_h);

    switch (w->type) {
    case DYN_STATUS:
        build_status(w);
        break;
    case DYN_KPI:
    case DYN_SPARK:
        build_kpi_like(w);
        break;
    case DYN_SERVER:
        build_server(w);
        break;
    case DYN_LIST:
        build_metric_list(w);
        break;
    case DYN_PET:
        ui_pet_create(&w->pet, w->body, w->body_w, w->body_h,
                      ui_theme()->pet_style);
        break;
    case DYN_MINI: {
        ui_mini_bind_t binds[UI_MINI_MAX_BINDS];
        ui_mini_row_t mini_rows[UI_MINI_MAX_ROWS];
        int nb = 0, nr = 0;
        for (int i = 0; i < w->binding_count; i++) {
            ui_mini_bind_t *b = &binds[nb++];
            memset(b, 0, sizeof(*b));
            copy_str(b->key, sizeof(b->key), w->bindings[i].key);
            b->metric = w->bindings[i].metric;
            copy_str(b->label, sizeof(b->label), w->bindings[i].label);
            copy_str(b->unit, sizeof(b->unit), w->bindings[i].unit);
            b->precision = w->bindings[i].precision;
            copy_str(b->mtype, sizeof(b->mtype), w->bindings[i].mtype);
        }
        for (int i = 0; i < w->row_count; i++) {
            ui_mini_row_t *r = &mini_rows[nr++];
            memset(r, 0, sizeof(*r));
            copy_str(r->label, sizeof(r->label), w->rows[i].label);
            r->metric = w->rows[i].metric;
            copy_str(r->kind, sizeof(r->kind), w->rows[i].kind);
            copy_str(r->unit, sizeof(r->unit), w->rows[i].unit);
            r->precision = w->rows[i].precision;
            copy_str(r->mtype, sizeof(r->mtype), w->rows[i].mtype);
        }
        w->mini = ui_mini_create(w->body, w->body_w, w->body_h, &w->mini_spec,
                                 w->w, w->h, binds, nb, mini_rows, nr);
        break;
    }
    default:
        break;
    }
    build_skin_deco(w);
    build_kpi_gauge(w);
}

/* ---------------- value formatting ---------------- */

static void format_metric(char *out, size_t cap, const dash_metric_t *m,
                          int precision, const char *unit)
{
    if (!m || !m->available) {
        snprintf(out, cap, "--");
        return;
    }
    if (m->type == DASH_METRIC_NUMBER) {
        if (precision < 0) precision = 0;
        if (precision > 3) precision = 3;
        char number[32];
        snprintf(number, sizeof(number), "%.*f", precision, m->number);
        if (unit && unit[0]) snprintf(out, cap, "%s %s", number, unit);
        else snprintf(out, cap, "%s", number);
    } else if (m->type == DASH_METRIC_BOOL) {
        snprintf(out, cap, "%s", m->boolean ? "是" : "否");
    } else {
        snprintf(out, cap, "%s", m->text[0] ? m->text : "--");
    }
}

static void format_number_only(char *out, size_t cap, const dash_metric_t *m,
                               int precision)
{
    if (!m || !m->available) {
        snprintf(out, cap, "--");
        return;
    }
    if (m->type == DASH_METRIC_NUMBER) {
        if (precision < 0) precision = 0;
        if (precision > 3) precision = 3;
        snprintf(out, cap, "%.*f", precision, m->number);
    } else if (m->type == DASH_METRIC_BOOL) {
        snprintf(out, cap, "%s", m->boolean ? "是" : "否");
    } else {
        snprintf(out, cap, "%s", m->text[0] ? m->text : "--");
    }
}

static void set_dot(lv_obj_t *dot, const dash_metric_t *m)
{
    if (!dot) return;
    uint32_t color = ui_theme()->text_mut;
    if (m && m->available && m->type == DASH_METRIC_BOOL) {
        color = m->boolean ? ui_theme()->ok : ui_theme()->err;
    }
    lv_color_t target = lv_color_hex(color);
    lv_color_t current = lv_obj_get_style_bg_color(dot, 0);
    if (current.red == target.red && current.green == target.green &&
        current.blue == target.blue) {
        return;
    }
    lv_obj_set_style_bg_color(dot, target, 0);
}

static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *old = lv_label_get_text(label);
    if (old && strcmp(old, text) == 0) return;
    lv_label_set_text(label, text);
}

static void apply_format_suffix(char *out, size_t cap, const char *value,
                                const char *format)
{
    if (!format || !format[0]) {
        snprintf(out, cap, "%s", value);
        return;
    }
    const char *mark = strstr(format, "{v}");
    if (mark) {
        int prefix = (int)(mark - format);
        snprintf(out, cap, "%.*s%s%s", prefix, format, value, mark + 3);
    } else {
        snprintf(out, cap, "%s %s", value, format);
    }
}

static void apply_widget(dyn_widget_t *w)
{
    char buf[128];
    char used[48], total[48];

    switch (w->type) {
    case DYN_STATUS:
        set_dot(w->status_dot, w->online_metric);
        if (w->status_model) {
            const char *text = (w->model_metric && w->model_metric->available)
                                   ? w->model_metric->text
                                   : "—";
            if (!text[0]) text = "—";
            label_set_if_changed(w->status_model, text);
            /* 模型名通常是英文：用 Montserrat，避免子集字库的拉丁字形被拉挤 */
            const lv_font_t *font = text_is_ascii(text) ? &lv_font_montserrat_14
                                                        : cjk_medium();
            if (lv_obj_get_style_text_font(w->status_model, 0) != font) {
                lv_obj_set_style_text_font(w->status_model, font, 0);
            }
        }
        if (w->status_phase) {
            const char *text = (w->phase_metric && w->phase_metric->available)
                                   ? w->phase_metric->text
                                   : "未知";
            if (!text[0]) text = "未知";
            label_set_if_changed(w->status_phase, text);
            const lv_font_t *font = text_is_ascii(text) ? &lv_font_montserrat_10
                                                        : cjk_small();
            if (lv_obj_get_style_text_font(w->status_phase, 0) != font) {
                lv_obj_set_style_text_font(w->status_phase, font, 0);
            }
        }
        break;

    case DYN_KPI:
    case DYN_SPARK: {
        format_number_only(buf, sizeof(buf), w->value_metric,
                           w->value_precision);
        label_set_if_changed(w->kpi_value, buf);
        const lv_font_t *vf = (w->w <= 4) ? &lv_font_montserrat_24
                                          : (strlen(buf) > 5 ? &lv_font_montserrat_24
                                                             : UI_F_H2);
        if (lv_obj_get_style_text_font(w->kpi_value, 0) != vf) {
            lv_obj_set_style_text_font(w->kpi_value, vf, 0);
        }
        float ratio = metric_ratio(w->value_metric, w->value_unit);
        if (ratio < 0.08f) ratio = 0.08f;
        if (w->kpi_gauge_arc) {
            lv_arc_set_value(w->kpi_gauge_arc, (int32_t)(ratio * 100.0f));
        }
        if (w->kpi_gauge_bar) {
            lv_obj_set_width(w->kpi_gauge_bar,
                             (int32_t)(w->body_w * ratio));
        }
        if (w->type == DYN_SPARK && w->chart && w->chart_series &&
            w->value_metric && w->value_metric->available &&
            w->value_metric->type == DASH_METRIC_NUMBER) {
            float v = (float)w->value_metric->number;
            bool first = (w->spark_count == 0);
            w->spark_hist[w->spark_next] = v;
            w->spark_next = (w->spark_next + 1) % 60;
            if (w->spark_count < 60) w->spark_count++;
            float max = 1.0f;
            float min = FLT_MAX;
            for (int i = 0; i < w->spark_count; i++) {
                float s = w->spark_hist[i];
                if (s > max) max = s;
                if (s < min) min = s;
            }
            if (min == FLT_MAX) min = 0.0f;
            float span = max - min;
            if (span < 1.0f) span = 1.0f;
            int lo = (int)(min - span * 0.15f);
            if (lo < 0) lo = 0;
            int hi = (int)(max + span * 0.15f) + 1;
            lv_chart_set_range(w->chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
            if (first) {
                lv_chart_set_all_values(w->chart, w->chart_series,
                                        (int32_t)(v + 0.5f));
            } else {
                lv_chart_set_next_value(w->chart, w->chart_series,
                                        (int32_t)(v + 0.5f));
            }
        }
        break;
    }

    case DYN_SERVER: {
        for (int i = 0; i < 4; i++) {
            const char *text = NULL;
            if (i == 1) {
                const dash_metric_t *u = w->server[i].metric;
                const dash_metric_t *t = w->ram_total_metric;
                if (!u || !t || !u->available || !t->available ||
                    u->type != DASH_METRIC_NUMBER ||
                    t->type != DASH_METRIC_NUMBER) {
                    text = "--";
                } else if (strcmp(w->server[i].unit, "MB") == 0 &&
                           strcmp(w->ram_total_unit, "MB") == 0) {
                    snprintf(buf, sizeof(buf), "%.1f / %.0f GB",
                             u->number / 1024.0, t->number / 1024.0);
                    text = buf;
                } else {
                    format_metric(used, sizeof(used), u,
                                  w->server[i].precision, w->server[i].unit);
                    format_metric(total, sizeof(total), t,
                                  w->ram_total_precision, w->ram_total_unit);
                    snprintf(buf, sizeof(buf), "%s / %s", used, total);
                    text = buf;
                }
            } else if (strcmp(w->server[i].key, "ip") == 0) {
                const dash_metric_t *m = w->server[i].metric;
                text = (m && m->available) ? m->text : "--";
            } else {
                format_metric(buf, sizeof(buf), w->server[i].metric,
                              w->server[i].precision, w->server[i].unit);
                text = buf;
            }
            if (w->server_value[i] && text) {
                label_set_if_changed(w->server_value[i], text);
            }
        }
        break;
    }

    case DYN_PET: {
        int state = UI_PET_OFFLINE;
        const dash_metric_t *m = w->value_metric;
        if (m && m->available) {
            const dash_metric_t *busy = w->pet_busy_metric;
            if (busy && busy->available && busy->type == DASH_METRIC_BOOL &&
                busy->boolean) {
                state = UI_PET_BUSY;
                break;
            }
            if (m->type == DASH_METRIC_BOOL) {
                state = m->boolean ? UI_PET_HAPPY : UI_PET_OFFLINE;
            } else if (m->type == DASH_METRIC_NUMBER) {
                if (m->number < w->pet_sleep_below) state = UI_PET_SLEEP;
                else if (m->number < w->pet_idle_below) state = UI_PET_IDLE;
                else if (m->number < w->pet_happy_below) state = UI_PET_HAPPY;
                else state = UI_PET_ALERT;
            } else {
                state = UI_PET_IDLE;
            }
        }
        ui_pet_set_state(&w->pet, state);
        break;
    }

    case DYN_LIST: {
        for (int i = 0; i < w->row_count; i++) {
            dyn_row_t *r = &w->rows[i];
            const dash_metric_t *m = r->metric;
            const char *text;
            if (!m || !m->available) {
                text = "--";
            } else if (m->type == DASH_METRIC_BOOL) {
                bool status_kind = (strcmp(r->kind, "status") == 0) ||
                                   (strcmp(r->kind, "auto") == 0);
                snprintf(buf, sizeof(buf), "%s",
                         status_kind ? (m->boolean ? "在线" : "离线")
                                     : (m->boolean ? "是" : "否"));
                text = buf;
            } else {
                format_metric(buf, sizeof(buf), m, r->precision, r->unit);
                text = buf;
            }
            if (r->format[0] && m && m->available &&
                m->type != DASH_METRIC_BOOL) {
                char formatted[256];
                apply_format_suffix(formatted, sizeof(formatted), text,
                                    r->format);
                if (w->list_value[i]) {
                    label_set_if_changed(w->list_value[i], formatted);
                }
            } else if (w->list_value[i]) {
                label_set_if_changed(w->list_value[i], text);
            }
            if (w->list_dot[i]) set_dot(w->list_dot[i], m);
        }
        break;
    }

    case DYN_MINI:
        if (w->mini) ui_mini_apply(w->mini);
        break;

    default:
        break;
    }
}

static void free_view(dyn_view_t *v, bool delete_screen)
{
    if (!v) return;
    if (v->classic) {
        ui_classic_destroy(v->classic); /* 屏幕随 classic 一起释放 */
    } else if (delete_screen && v->screen) {
        lv_obj_del(v->screen);
    }
    if (v->widgets) {
        for (int i = 0; i < v->widget_count; i++) {
            if (v->widgets[i].mini) ui_mini_destroy(v->widgets[i].mini);
        }
        heap_caps_free(v->widgets);
    }
    free(v);
}

void ui_dynamic_apply(void)
{
    if (!s_view) return;
    int64_t t0 = esp_timer_get_time();
    if (s_view->classic) {
        ui_classic_apply(s_view->classic);
    } else {
        for (int i = 0; i < s_view->widget_count; i++) {
            apply_widget(&s_view->widgets[i]);
        }
    }
    int64_t dt = esp_timer_get_time() - t0;
    s_apply_last_us = dt;
    s_apply_sum_us += dt;
    s_apply_count++;
    if (dt > s_apply_max_us) s_apply_max_us = dt;
}

void ui_dynamic_perf(ui_dynamic_perf_t *out)
{
    if (!out) return;
    out->apply_count = s_apply_count;
    out->apply_last_us = s_apply_last_us;
    out->apply_max_us = s_apply_max_us;
    out->apply_avg_us = s_apply_count ? s_apply_sum_us / s_apply_count : 0;
}

bool ui_dynamic_active(void)
{
    return s_view != NULL;
}

bool ui_dynamic_engaged(void)
{
    return s_engaged;
}

int ui_dynamic_widget_count(void)
{
    return s_view ? s_view->widget_count : 0;
}

void ui_dynamic_commit(void)
{
    if (!s_staging) return;
    lv_screen_load(s_staging->screen);
    if (s_pending_blank) {
        lv_obj_del(s_pending_blank);
        s_pending_blank = NULL;
    }
    dyn_view_t *old = s_view;
    s_view = s_staging;
    s_staging = NULL;
    if (old) {
        free_view(old, true);
        ESP_LOGI(TAG, "layout committed atomically");
    }
    s_engaged = true;
    pet_timer_sync();
    deco_timer_sync();
    ui_dynamic_apply();
}

void ui_dynamic_destroy(void)
{
    if (s_pet_timer) {
        lv_timer_delete(s_pet_timer);
        s_pet_timer = NULL;
    }
    if (s_deco_timer) {
        lv_timer_delete(s_deco_timer);
        s_deco_timer = NULL;
    }
    if (s_staging) {
        free_view(s_staging, true);
        s_staging = NULL;
    }
    if (s_view) {
        lv_obj_t *blank = lv_obj_create(NULL);
        if (blank) {
            lv_obj_remove_style_all(blank);
            lv_obj_set_style_bg_color(blank, lv_color_hex(ui_theme()->bg), 0);
            lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
            lv_screen_load(blank);
        }
        free_view(s_view, true);
        s_view = NULL;
        s_pending_blank = blank;
    }
}

void ui_dynamic_show_portal(bool show, const char *ssid, const char *pass,
                            const char *ip)
{
    if (!show) {
        if (s_portal_overlay) {
            lv_obj_add_flag(s_portal_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!s_portal_overlay) {
        lv_obj_t *top = lv_layer_top();
        s_portal_overlay = lv_obj_create(top);
        lv_obj_remove_style_all(s_portal_overlay);
        lv_obj_set_pos(s_portal_overlay, 0, 0);
        lv_obj_set_size(s_portal_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_portal_overlay, lv_color_hex(ui_theme()->bg), 0);
        lv_obj_set_style_bg_opa(s_portal_overlay, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_portal_overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *card = ui_card_create(s_portal_overlay);
        lv_obj_set_size(card, 280, 260);
        lv_obj_center(card);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_pad_row(card, 8, 0);

        make_label(card, "配网模式", cjk_large(), ui_theme()->accent);
        make_label(card, "连接热点后打开浏览器", cjk_large(), ui_theme()->text_mut);
        s_portal_ssid = make_label(card, "Wi-Fi: ", cjk_large(), ui_theme()->text);
        lv_obj_set_width(s_portal_ssid, LV_PCT(100));
        lv_label_set_long_mode(s_portal_ssid, LV_LABEL_LONG_MODE_DOTS);
        s_portal_pass = make_label(card, "密码: ", cjk_large(), ui_theme()->text);
        lv_obj_set_width(s_portal_pass, LV_PCT(100));
        lv_label_set_long_mode(s_portal_pass, LV_LABEL_LONG_MODE_DOTS);
        s_portal_url = make_label(card, "http://192.168.4.1", UI_F_H3,
                                  ui_theme()->accent);
        make_label(card, "保存后设备自动重启并连接", cjk_large(), ui_theme()->text_mut);
    }

    lv_label_set_text_fmt(s_portal_ssid, "Wi-Fi: %s", ssid ? ssid : "");
    lv_label_set_text_fmt(s_portal_pass, "密码: %s", pass ? pass : "");
    lv_label_set_text_fmt(s_portal_url, "http://%s",
                          ip ? ip : "192.168.4.1");
    lv_obj_remove_flag(s_portal_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_portal_overlay);
    if (s_network_overlay) {
        lv_obj_add_flag(s_network_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_dynamic_show_network(bool show, const char *ssid, const char *ip,
                             const char *device_id, const char *name,
                             bool server_connected)
{
    if (!show) {
        if (s_network_overlay) {
            lv_obj_add_flag(s_network_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!s_network_overlay) {
        lv_obj_t *top = lv_layer_top();
        s_network_overlay = lv_obj_create(top);
        lv_obj_remove_style_all(s_network_overlay);
        lv_obj_set_pos(s_network_overlay, 0, 0);
        lv_obj_set_size(s_network_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_network_overlay, lv_color_hex(ui_theme()->bg), 0);
        lv_obj_set_style_bg_opa(s_network_overlay, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_network_overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *card = ui_card_create(s_network_overlay);
        lv_obj_set_size(card, 292, 272);
        lv_obj_center(card);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_pad_row(card, 6, 0);

        make_label(card, "网络已连接", cjk_large(), ui_theme()->accent);
        s_net_ip = make_label(card, "IP: --", &lv_font_montserrat_32,
                              ui_theme()->text);
        s_net_ssid = make_label(card, "Wi-Fi: --", cjk_small(),
                                ui_theme()->text_mut);
        s_net_name = make_label(card, "名称: --", cjk_small(),
                                ui_theme()->text_dim);
        s_net_id = make_label(card, "ID: --", &lv_font_montserrat_12,
                              ui_theme()->text_mut);
        s_net_state = make_label(card, "等待服务端连接", cjk_small(),
                                 ui_theme()->warn);
        s_net_hint = make_label(card,
                                "在服务端输入屏幕 IP\n连接后自动显示看板",
                                cjk_small(), ui_theme()->text_mut);
        lv_obj_t *labels[] = {s_net_ssid, s_net_name, s_net_state};
        for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
            lv_obj_set_width(labels[i], LV_PCT(100));
            lv_label_set_long_mode(labels[i], LV_LABEL_LONG_MODE_DOTS);
        }
        lv_obj_set_width(s_net_hint, LV_PCT(100));
        lv_label_set_long_mode(s_net_hint, LV_LABEL_LONG_MODE_WRAP);
    }

    lv_label_set_text_fmt(s_net_ip, "IP: %s", (ip && ip[0]) ? ip : "--");
    lv_label_set_text_fmt(s_net_ssid, "Wi-Fi: %s",
                          (ssid && ssid[0]) ? ssid : "--");
    lv_label_set_text_fmt(s_net_name, "名称: %s",
                          (name && name[0]) ? name : "未命名");
    lv_label_set_text_fmt(s_net_id, "ID: %s",
                          (device_id && device_id[0]) ? device_id : "--");
    lv_label_set_text(s_net_state,
                      server_connected ? "服务端已连接" : "等待服务端连接");
    lv_obj_set_style_text_color(
        s_net_state,
        lv_color_hex(server_connected ? ui_theme()->ok : ui_theme()->warn), 0);
    lv_obj_remove_flag(s_network_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_network_overlay);
    if (s_portal_overlay) {
        lv_obj_add_flag(s_portal_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- public build ---------------- */

bool ui_dynamic_build(const char *layout_json, char *subscribe_out,
                      size_t subscribe_cap)
{
    if (!layout_json || !subscribe_out || subscribe_cap < 32) return false;
    if (s_staging) {
        free_view(s_staging, true);
        s_staging = NULL;
    }

    bool ok = false;
    cJSON *root = cJSON_Parse(layout_json);
    dyn_view_t *next = calloc(1, sizeof(dyn_view_t));
    if (!next) {
        ESP_LOGE(TAG, "no RAM for layout staging");
        cJSON_Delete(root);
        return false;
    }
    if (!root) {
        ESP_LOGE(TAG, "layout JSON parse failed");
        goto out;
    }

    parse_theme_tokens(root);

    dyn_canvas_t canvas = s_canvas;
    if (!canvas_from_json(root, &canvas)) {
        const char *profile = jstr(root, "profile", "");
        if (!canvas_from_profile(profile, &canvas)) {
            ESP_LOGW(TAG, "unknown profile '%s', keep %dx%d canvas", profile,
                     s_canvas.w, s_canvas.h);
        }
    }
    s_canvas = canvas;

    /* 传统框架：固定式界面主题整屏渲染，指标订阅为固定集合 */
    if (strcmp(jstr(root, "framework", "widgets"), "classic") == 0) {
        const char *ct = jstr(root, "classic_theme", "instrument");
        ui_classic_t *classic = ui_classic_build(ct, canvas.w, canvas.h);
        if (!classic) {
            ESP_LOGE(TAG, "classic theme build failed: %s", ct);
            goto out;
        }
        char cids[DYN_MAX_SUB_IDS][DASH_METRIC_ID_LEN];
        int cid_count = 0;
        int n_ids = 0;
        const char **ids = ui_classic_metric_ids(&n_ids);
        for (int i = 0; i < n_ids && cid_count < DYN_MAX_SUB_IDS; i++) {
            copy_str(cids[cid_count++], DASH_METRIC_ID_LEN, ids[i]);
        }
        build_subscribe(cids, cid_count, subscribe_out, subscribe_cap);
        next->classic = classic;
        next->screen = ui_classic_screen(classic);
        s_staging = next;
        next = NULL;
        ok = true;
        ESP_LOGI(TAG, "classic UI staged: theme=%s, %d metrics", ct,
                 cid_count);
        goto out;
    }

    int version = jint(root, "v", 1);
    cJSON *widgets_json = NULL;

    if (version == 1) {
        widgets_json = cJSON_GetObjectItemCaseSensitive(root, "widgets");
    } else if (version == 2) {
        cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) < 1) {
            ESP_LOGE(TAG, "v2 layout has no page");
            goto out;
        }
        if (cJSON_GetArraySize(pages) > 1) {
            ESP_LOGW(TAG, "single-page mode: ignoring %d extra page(s)",
                     cJSON_GetArraySize(pages) - 1);
        }
        cJSON *page = cJSON_GetArrayItem(pages, 0);
        if (cJSON_IsObject(page)) {
            widgets_json = cJSON_GetObjectItemCaseSensitive(page, "widgets");
        }
    } else {
        ESP_LOGE(TAG, "unsupported layout version %d", version);
        goto out;
    }
    if (!cJSON_IsArray(widgets_json)) {
        ESP_LOGE(TAG, "layout has no widgets");
        goto out;
    }

    char ids[DYN_MAX_SUB_IDS][DASH_METRIC_ID_LEN];
    memset(ids, 0, sizeof(ids));
    int id_count = 0;
    int n = cJSON_GetArraySize(widgets_json);
    for (int i = 0; i < n; i++) {
        cJSON *wj = cJSON_GetArrayItem(widgets_json, i);
        if (cJSON_IsObject(wj)) collect_widget_ids(wj, ids, &id_count);
    }
    build_subscribe(ids, id_count, subscribe_out, subscribe_cap);

    next->widgets = heap_caps_calloc(DYN_MAX_WIDGETS, sizeof(dyn_widget_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next->widgets) {
        ESP_LOGE(TAG, "no PSRAM for dynamic widgets");
        goto out;
    }

    for (int i = 0; i < n && next->widget_count < DYN_MAX_WIDGETS; i++) {
        cJSON *wj = cJSON_GetArrayItem(widgets_json, i);
        if (!cJSON_IsObject(wj)) continue;
        dyn_widget_t parsed;
        if (!parse_widget(wj, &parsed)) {
            ESP_LOGW(TAG, "skip unsupported widget at index %d", i);
            continue;
        }
        next->widgets[next->widget_count++] = parsed;
    }

    if (next->widget_count == 0) {
        ESP_LOGE(TAG, "no supported widgets");
        goto out;
    }

    next->screen = lv_obj_create(NULL);
    if (!next->screen) {
        ESP_LOGE(TAG, "screen create failed");
        goto out;
    }
    lv_obj_remove_style_all(next->screen);
    lv_obj_set_style_bg_color(next->screen, lv_color_hex(ui_theme()->bg), 0);
    lv_obj_set_style_bg_opa(next->screen, LV_OPA_COVER, 0);
    if (ui_theme()->skin != UI_SKIN_MINIMAL) {
        lv_obj_set_style_bg_grad_color(next->screen,
                                       lv_color_hex(ui_theme()->surface_grad_to), 0);
        lv_obj_set_style_bg_grad_dir(next->screen, LV_GRAD_DIR_VER, 0);
    }
    lv_obj_remove_flag(next->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(next->screen, screen_deco_cb,
                        LV_EVENT_DRAW_MAIN_END, NULL);

    for (int i = 0; i < next->widget_count; i++) {
        build_widget_ui(&next->widgets[i], next->screen);
    }
    deco_create(next);

    s_staging = next;
    ok = true;
    ESP_LOGI(TAG, "dynamic UI staged: %d widgets, %d subscribed metrics",
             next->widget_count, id_count);

out:
    if (!ok) {
        free_view(next, true);
    }
    cJSON_Delete(root);
    return ok;
}

static int count_supported_widgets(cJSON *arr)
{
    if (!cJSON_IsArray(arr)) return 0;
    int n = 0;
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON *w = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(w)) continue;
        const char *type = jstr(w, "type", "");
        if (ui_mini_is_type(type) || strcmp(type, "status_pill") == 0 ||
            strcmp(type, "kpi") == 0 || strcmp(type, "sparkline") == 0 ||
            strcmp(type, "server_card") == 0 ||
            strcmp(type, "metric_list") == 0 ||
            strcmp(type, "desktop_pet") == 0) {
            n++;
        }
    }
    return n;
}

int ui_dynamic_json_widget_count(const char *layout_json)
{
    if (!layout_json) return 0;
    cJSON *root = cJSON_Parse(layout_json);
    if (!root) return 0;

    int total = 0;
    int version = jint(root, "v", 1);
    if (version == 1) {
        total = count_supported_widgets(
            cJSON_GetObjectItemCaseSensitive(root, "widgets"));
    } else if (version == 2) {
        cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        if (cJSON_IsArray(pages)) {
            for (int i = 0; i < cJSON_GetArraySize(pages); i++) {
                cJSON *page = cJSON_GetArrayItem(pages, i);
                if (cJSON_IsObject(page)) {
                    total += count_supported_widgets(
                        cJSON_GetObjectItemCaseSensitive(page, "widgets"));
                }
            }
        }
    }
    cJSON_Delete(root);
    return total;
}
