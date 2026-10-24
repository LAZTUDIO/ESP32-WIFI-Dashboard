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

static const char *TAG = "uidyn";

#define DYN_MAX_WIDGETS 32
#define DYN_MAX_BINDINGS 8
#define DYN_MAX_ROWS 8
#define DYN_MAX_SUB_IDS 64

#define PROFILE_W 320
#define PROFILE_H 480
#define GRID_COLS 12
#define GRID_ROWS 16
#define GRID_GAP 6
#define GRID_PAD 8

enum {
    DYN_STATUS = 0,
    DYN_KPI,
    DYN_SPARK,
    DYN_SERVER,
    DYN_LIST,
    DYN_PET,
    DYN_UNKNOWN,
};

enum {
    PET_SLEEP = 0,
    PET_IDLE,
    PET_HAPPY,
    PET_BUSY,
    PET_ALERT,
    PET_OFFLINE,
};

enum {
    PAGE_ANIM_NONE = 0,
    PAGE_ANIM_SLIDE,
    PAGE_ANIM_FADE,
    PAGE_ANIM_STACK,
    PAGE_ANIM_ZOOM,
    PAGE_ANIM_FLIP,
    PAGE_ANIM_ROTATE,
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
    int page;
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
    int pet_state;
    uint32_t pet_t0;
    int pet_base_x, pet_base_y;
    int pet_eye_x1, pet_eye_x2, pet_eye_y, pet_eye_w, pet_eye_h;
    int pet_mouth_x, pet_mouth_y, pet_mouth_w, pet_mouth_h;
    int pet_tail_x, pet_tail_y;
    lv_obj_t *pet_actor;
    lv_obj_t *pet_head;
    lv_obj_t *pet_torso;
    lv_obj_t *pet_ear_l;
    lv_obj_t *pet_ear_r;
    lv_obj_t *pet_eye_l;
    lv_obj_t *pet_eye_r;
    lv_obj_t *pet_cheek_l;
    lv_obj_t *pet_cheek_r;
    lv_obj_t *pet_mouth;
    lv_obj_t *pet_tail;
    lv_obj_t *pet_fx;
} dyn_widget_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *tileview;
    lv_obj_t *tiles[4];
    int page_count;
    int page_anim;
    int current_page;
    bool pressed;
    int press_x;
    int press_y;
    lv_obj_t *trans_layer;
    lv_draw_buf_t *trans_buf;
    dyn_widget_t *widgets;
    int widget_count;
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
static int64_t s_page_t0;
static int64_t s_page_last_ms;
static uint32_t s_page_swaps;
static int64_t s_trans_last_ms;
static uint32_t s_trans_t0;
static lv_timer_t *s_pet_timer;

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

static bool jbool(cJSON *obj, const char *key, bool def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return def;
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
    }
    ui_theme_set(&theme);
    const char *id = jstr(root, "theme", "classic");
    ESP_LOGI(TAG, "theme '%s' bg=#%06X accent=#%06X radius=%d", id,
             (unsigned)theme.bg, (unsigned)theme.accent, theme.radius);
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
    w->type = type_from_name(jstr(wj, "type", ""));
    if (w->type == DYN_UNKNOWN) return false;

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
    w->pet_state = -1;

    cJSON *options = cJSON_GetObjectItemCaseSensitive(wj, "options");
    cJSON *labels = NULL, *units = NULL, *precisions = NULL, *types = NULL;
    if (cJSON_IsObject(options)) {
        w->show_title = jbool(options, "show_title", true);
        copy_str(w->title, sizeof(w->title), jstr(options, "title", type_label(w->type)));
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
        copy_str(w->title, sizeof(w->title), type_label(w->type));
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
    } else if (w->type == DYN_LIST) {
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
    const float cw = (PROFILE_W - 2.0f * GRID_PAD -
                      (GRID_COLS - 1) * GRID_GAP) / GRID_COLS;
    const float ch = (PROFILE_H - 2.0f * GRID_PAD -
                      (GRID_ROWS - 1) * GRID_GAP) / GRID_ROWS;
    *ox = (int)lroundf(GRID_PAD + gx * (cw + GRID_GAP));
    *oy = (int)lroundf(GRID_PAD + gy * (ch + GRID_GAP));
    *ow = (int)lroundf(gw * cw + (gw - 1) * GRID_GAP);
    *oh = (int)lroundf(gh * ch + (gh - 1) * GRID_GAP);
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
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ui_theme()->text_mut), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
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
    const lv_font_t *value_font =
        (w->w <= 4) ? &lv_font_montserrat_24 : UI_F_H2;
    w->kpi_value = make_label(row, "--", value_font, ui_theme()->text);
    lv_label_set_long_mode(w->kpi_value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(w->kpi_value, LV_PCT(72), 0);
    w->kpi_unit = make_label(row, w->value_unit, &lv_font_montserrat_12, ui_theme()->text_mut);

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

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = ui_group_create(w->body);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, body_w, row_h);

        w->server_label[i] = make_label(row, w->server[i].label, cjk_medium(),
                                        ui_theme()->text_mut);
        lv_obj_set_pos(w->server_label[i], 0, label_y > 0 ? label_y : 0);
        lv_obj_set_size(w->server_label[i], label_w,
                        cjk_medium()->line_height);
        lv_label_set_long_mode(w->server_label[i], LV_LABEL_LONG_MODE_DOTS);

        w->server_value[i] = make_label(row, "--", &lv_font_montserrat_12, ui_theme()->text);
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

    for (int i = 0; i < w->row_count; i++) {
        dyn_row_t *r = &w->rows[i];
        lv_obj_t *row = ui_group_create(w->body);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, body_w, row_h);

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
            w->list_label[i] = make_label(row, r->label, label_font, ui_theme()->text_mut);
            lv_obj_set_pos(w->list_label[i], x, label_y);
            lv_obj_set_size(w->list_label[i], label_w, label_font->line_height);
            lv_label_set_long_mode(w->list_label[i], LV_LABEL_LONG_MODE_DOTS);
            x += label_w;
        }
        int value_y = (row_h - row_value_font->line_height) / 2;
        if (value_y < 0) value_y = 0;
        w->list_value[i] = make_label(row, "--", row_value_font, ui_theme()->text);
        lv_obj_set_pos(w->list_value[i], x, value_y);
        lv_obj_set_size(w->list_value[i], body_w - x,
                        row_value_font->line_height);
        lv_obj_set_style_text_align(w->list_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(w->list_value[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

/* ---------------- 桌宠 ---------------- */

static lv_obj_t *make_pet_part(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void pet_place(lv_obj_t *o, int x, int y, int w, int h)
{
    if (!o) return;
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
}

static void build_pet(dyn_widget_t *w)
{
    const ui_theme_t *th = ui_theme();
    int bw = w->body_w;
    int bh = w->body_h;
    int s = bw < bh ? bw : bh;
    if (s > 120) s = 120;
    if (s < 36) s = 36;
    int ox = (bw - s) / 2;
    int oy = bh - s;
    if (oy < 0) oy = 0;
    w->pet_base_x = ox;
    w->pet_base_y = oy;

    lv_obj_t *shadow = make_pet_part(w->body, th->text_mut);
    pet_place(shadow, ox + s * 23 / 100, oy + s * 90 / 100,
              s * 54 / 100, s * 7 / 100);
    lv_obj_set_style_bg_opa(shadow, LV_OPA_30, 0);

    w->pet_actor = ui_group_create(w->body);
    lv_obj_set_pos(w->pet_actor, ox, oy);
    lv_obj_set_size(w->pet_actor, s, s);

    w->pet_ear_l = make_pet_part(w->pet_actor, th->accent_2);
    w->pet_ear_r = make_pet_part(w->pet_actor, th->accent_2);
    w->pet_torso = make_pet_part(w->pet_actor, th->accent);
    w->pet_head = make_pet_part(w->pet_actor, th->accent);
    w->pet_cheek_l = make_pet_part(w->pet_actor, th->pink);
    w->pet_cheek_r = make_pet_part(w->pet_actor, th->pink);
    w->pet_eye_l = make_pet_part(w->pet_actor, th->bg);
    w->pet_eye_r = make_pet_part(w->pet_actor, th->bg);
    w->pet_mouth = make_pet_part(w->pet_actor, th->bg);
    w->pet_tail = make_pet_part(w->pet_actor, th->accent_2);

    pet_place(w->pet_ear_l, s * 18 / 100, s * 3 / 100, s * 15 / 100, s * 15 / 100);
    pet_place(w->pet_ear_r, s * 67 / 100, s * 3 / 100, s * 15 / 100, s * 15 / 100);
    pet_place(w->pet_torso, s * 17 / 100, s * 50 / 100, s * 66 / 100, s * 44 / 100);
    pet_place(w->pet_head, s * 21 / 100, s * 12 / 100, s * 58 / 100, s * 50 / 100);
    pet_place(w->pet_cheek_l, s * 25 / 100, s * 48 / 100, s * 9 / 100, s * 5 / 100);
    pet_place(w->pet_cheek_r, s * 66 / 100, s * 48 / 100, s * 9 / 100, s * 5 / 100);
    lv_obj_set_style_bg_opa(w->pet_cheek_l, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(w->pet_cheek_r, LV_OPA_60, 0);

    w->pet_eye_x1 = s * 35 / 100;
    w->pet_eye_x2 = s * 58 / 100;
    w->pet_eye_y = s * 33 / 100;
    w->pet_eye_w = s * 7 / 100;
    if (w->pet_eye_w < 3) w->pet_eye_w = 3;
    w->pet_eye_h = s * 10 / 100;
    if (w->pet_eye_h < 4) w->pet_eye_h = 4;
    pet_place(w->pet_eye_l, w->pet_eye_x1, w->pet_eye_y,
              w->pet_eye_w, w->pet_eye_h);
    pet_place(w->pet_eye_r, w->pet_eye_x2, w->pet_eye_y,
              w->pet_eye_w, w->pet_eye_h);

    w->pet_mouth_w = s * 14 / 100;
    if (w->pet_mouth_w < 6) w->pet_mouth_w = 6;
    w->pet_mouth_h = s * 4 / 100;
    if (w->pet_mouth_h < 2) w->pet_mouth_h = 2;
    w->pet_mouth_x = s / 2 - w->pet_mouth_w / 2;
    w->pet_mouth_y = s * 53 / 100;
    pet_place(w->pet_mouth, w->pet_mouth_x, w->pet_mouth_y,
              w->pet_mouth_w, w->pet_mouth_h);

    w->pet_tail_x = s * 80 / 100;
    w->pet_tail_y = s * 60 / 100;
    pet_place(w->pet_tail, w->pet_tail_x, w->pet_tail_y,
              s * 12 / 100, s * 12 / 100);

    w->pet_fx = lv_label_create(w->pet_actor);
    lv_label_set_text(w->pet_fx, "");
    lv_obj_set_style_text_font(w->pet_fx, UI_F_CAP, 0);
    lv_obj_set_style_text_color(w->pet_fx, lv_color_hex(th->text_dim), 0);
    lv_obj_set_pos(w->pet_fx, s * 66 / 100, 0);
    lv_obj_set_size(w->pet_fx, s * 34 / 100, 16);
    lv_obj_set_style_text_align(w->pet_fx, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(w->pet_fx, LV_OPA_TRANSP, 0);

    w->pet_t0 = lv_tick_get();
    if (w->pet_t0 == 0) w->pet_t0 = 1;
}

static void pet_apply_state(dyn_widget_t *w, int state)
{
    if (!w->pet_actor || w->pet_state == state) return;
    w->pet_state = state;
    w->pet_t0 = lv_tick_get();
    if (w->pet_t0 == 0) w->pet_t0 = 1;

    const ui_theme_t *th = ui_theme();
    uint32_t body = th->accent;
    uint32_t second = th->accent_2;
    uint32_t eye = th->bg;
    const char *fx = "";
    switch (state) {
    case PET_SLEEP:
        body = th->accent_2;
        fx = "Zzz";
        break;
    case PET_HAPPY:
        break;
    case PET_BUSY:
        fx = "...";
        break;
    case PET_ALERT:
        body = th->err;
        eye = th->err;
        fx = "!";
        break;
    case PET_OFFLINE:
        body = th->text_mut;
        second = th->text_dim;
        eye = th->text_mut;
        fx = "?";
        break;
    default:
        break;
    }

    lv_obj_set_style_bg_color(w->pet_head, lv_color_hex(body), 0);
    lv_obj_set_style_bg_color(w->pet_torso, lv_color_hex(body), 0);
    lv_obj_set_style_bg_color(w->pet_ear_l, lv_color_hex(second), 0);
    lv_obj_set_style_bg_color(w->pet_ear_r, lv_color_hex(second), 0);
    lv_obj_set_style_bg_color(w->pet_tail, lv_color_hex(second), 0);
    lv_obj_set_style_bg_color(w->pet_eye_l, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_color(w->pet_eye_r, lv_color_hex(eye), 0);
    lv_obj_set_style_bg_opa(w->pet_cheek_l,
                            state == PET_OFFLINE ? LV_OPA_20 : LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(w->pet_cheek_r,
                            state == PET_OFFLINE ? LV_OPA_20 : LV_OPA_60, 0);
    lv_label_set_text(w->pet_fx, fx);
    lv_obj_set_style_text_color(w->pet_fx,
        lv_color_hex(state == PET_ALERT ? th->err : th->text_dim), 0);
    lv_obj_set_style_opa(w->pet_fx, fx[0] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void pet_animate(dyn_widget_t *w, uint32_t now)
{
    if (!w->pet_actor) return;
    float t = (now - w->pet_t0) / 1000.0f;
    int dx = 0, dy = 0;
    int eye_y = w->pet_eye_y;
    int eye_h = w->pet_eye_h;
    int mouth_w = w->pet_mouth_w;
    int tail_dx = 0;
    bool blink = false;

    switch (w->pet_state) {
    case PET_IDLE:
        dy = (int)(sinf(t * 2.4f) * 2.0f);
        tail_dx = (int)(sinf(t * 1.8f) * 2.0f);
        blink = fmodf(t, 3.6f) < 0.12f;
        break;
    case PET_HAPPY:
        dy = -(int)(fabsf(sinf(t * 5.0f)) * 5.0f);
        mouth_w += 3;
        tail_dx = (int)(sinf(t * 9.0f) * 4.0f);
        blink = fmodf(t, 2.4f) < 0.1f;
        break;
    case PET_BUSY:
        dx = (int)(sinf(t * 16.0f) * 1.5f);
        tail_dx = (int)(sinf(t * 6.0f) * 3.0f);
        break;
    case PET_ALERT:
        dx = (int)(sinf(t * 38.0f) * 2.0f);
        eye_h += 2;
        eye_y -= 1;
        tail_dx = (int)(sinf(t * 13.0f) * 5.0f);
        break;
    case PET_SLEEP:
        dy = (int)(sinf(t * 1.1f) * 1.5f);
        eye_h = 2;
        eye_y = w->pet_eye_y + w->pet_eye_h / 2;
        mouth_w -= 3;
        tail_dx = -2;
        break;
    case PET_OFFLINE:
        eye_h = 2;
        eye_y = w->pet_eye_y + w->pet_eye_h / 2;
        mouth_w -= 3;
        break;
    default:
        break;
    }

    if (blink) {
        eye_h = 2;
        eye_y = w->pet_eye_y + w->pet_eye_h / 2;
    }
    if (mouth_w < 5) mouth_w = 5;

    lv_obj_set_pos(w->pet_actor, w->pet_base_x + dx, w->pet_base_y + dy);
    lv_obj_set_pos(w->pet_eye_l, w->pet_eye_x1, eye_y);
    lv_obj_set_size(w->pet_eye_l, w->pet_eye_w, eye_h);
    lv_obj_set_pos(w->pet_eye_r, w->pet_eye_x2, eye_y);
    lv_obj_set_size(w->pet_eye_r, w->pet_eye_w, eye_h);
    lv_obj_set_pos(w->pet_mouth,
                   w->pet_mouth_x + (w->pet_mouth_w - mouth_w) / 2,
                   w->pet_mouth_y);
    lv_obj_set_size(w->pet_mouth, mouth_w, w->pet_mouth_h);
    lv_obj_set_pos(w->pet_tail, w->pet_tail_x + tail_dx, w->pet_tail_y);
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
        if (w->type == DYN_PET && w->page == s_view->current_page) {
            pet_animate(w, now);
        }
    }
}

static void pet_timer_sync(void)
{
    bool any = view_has_pet(s_view);
    if (any && !s_pet_timer) {
        s_pet_timer = lv_timer_create(pet_timer_cb, 50, NULL);
    } else if (!any && s_pet_timer) {
        lv_timer_delete(s_pet_timer);
        s_pet_timer = NULL;
    }
}

static void enable_gesture_bubble(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        enable_gesture_bubble(lv_obj_get_child(obj, (int32_t)i));
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
    w->root = card;

    int header_h = 0;
    if (w->show_title) {
        lv_obj_t *title = make_label(card, w->title, cjk_small(), ui_theme()->text_dim);
        lv_obj_set_pos(title, 8, 4);
        lv_obj_set_size(title, ww - 16, 18);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        header_h = 22;
    }

    w->body = ui_group_create(card);
    int body_y = header_h ? header_h + 2 : 6;
    int body_h = hh - body_y - 6;
    if (body_h < 1) body_h = 1;
    w->body_w = ww - 12;
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
        build_pet(w);
        break;
    default:
        break;
    }
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
        int state = PET_OFFLINE;
        const dash_metric_t *m = w->value_metric;
        if (m && m->available) {
            const dash_metric_t *busy = w->pet_busy_metric;
            if (busy && busy->available && busy->type == DASH_METRIC_BOOL &&
                busy->boolean) {
                state = PET_BUSY;
            } else if (m->type == DASH_METRIC_BOOL) {
                state = m->boolean ? PET_HAPPY : PET_OFFLINE;
            } else if (m->type == DASH_METRIC_NUMBER) {
                if (m->number < w->pet_sleep_below) state = PET_SLEEP;
                else if (m->number < w->pet_idle_below) state = PET_IDLE;
                else if (m->number < w->pet_happy_below) state = PET_HAPPY;
                else state = PET_ALERT;
            } else {
                state = PET_IDLE;
            }
        }
        pet_apply_state(w, state);
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

    default:
        break;
    }
}

static int active_page_index(dyn_view_t *v)
{
    if (!v || !v->tileview) return -1;
    lv_obj_t *active = lv_tileview_get_tile_active(v->tileview);
    for (int i = 0; i < v->page_count; i++) {
        if (v->tiles[i] == active) return i;
    }
    return -1;
}

static lv_obj_t *make_trans_image(dyn_view_t *v, lv_draw_buf_t *buf)
{
    lv_obj_t *img = lv_image_create(v->screen);
    lv_image_set_src(img, buf);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_size(img, PROFILE_W, PROFILE_H);
    lv_image_set_pivot(img, PROFILE_W / 2, PROFILE_H / 2);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(img);
    return img;
}

static void trans_fade_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)(255 - 255 * v / 100), 0);
}

static void trans_stack_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    lv_image_set_scale(o, 256 - 76 * v / 100);
    lv_obj_set_y(o, PROFILE_H * v / 100);
}

static void trans_zoom_cb(void *var, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)var, 256 - 216 * v / 100);
}

static void trans_rotate_cb(void *var, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)var, 256 - 56 * v / 100);
    lv_image_set_rotation((lv_obj_t *)var, -300 * v / 100);
}

static void trans_flip_cb(void *var, int32_t v)
{
    int sx = 256 - 255 * v / 100;
    lv_image_set_scale_x((lv_obj_t *)var, sx < 1 ? 1 : sx);
}

static void trans_clear(dyn_view_t *v)
{
    if (!v) return;
    if (v->trans_layer) {
        lv_anim_delete(v->trans_layer, NULL);
        lv_obj_del(v->trans_layer);
        v->trans_layer = NULL;
    }
    if (v->trans_buf) {
        lv_draw_buf_destroy(v->trans_buf);
        v->trans_buf = NULL;
    }
}

static void trans_ready_cb(lv_anim_t *a)
{
    lv_obj_t *old = (lv_obj_t *)a->var;
    s_trans_last_ms = lv_tick_elaps(s_trans_t0);
    lv_draw_buf_t *buf = NULL;
    if (s_view && s_view->trans_layer == old) {
        s_view->trans_layer = NULL;
        buf = s_view->trans_buf;
        s_view->trans_buf = NULL;
    }
    if (old) lv_obj_del(old);
    if (buf) lv_draw_buf_destroy(buf);
    ESP_LOGI(TAG, "page transition %lld ms", (long long)s_trans_last_ms);
}

static void start_transition(dyn_view_t *v, lv_draw_buf_t *snap, int from,
                             int to)
{
    (void)from;
    (void)to;
    if (!v || !v->screen || !snap) return;

    lv_obj_t *old = make_trans_image(v, snap);
    v->trans_layer = old;
    v->trans_buf = snap;

    lv_anim_exec_xcb_t exec = trans_fade_cb;
    int duration = 200;
    int end = 100;

    if (v->page_anim == PAGE_ANIM_STACK) {
        exec = trans_stack_cb;
        duration = 240;
    } else if (v->page_anim == PAGE_ANIM_ZOOM) {
        exec = trans_zoom_cb;
        duration = 240;
    } else if (v->page_anim == PAGE_ANIM_ROTATE) {
        exec = trans_rotate_cb;
        duration = 260;
    } else if (v->page_anim == PAGE_ANIM_FLIP) {
        exec = trans_flip_cb;
        duration = 180;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, old);
    lv_anim_set_values(&a, 0, end);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_ready_cb(&a, trans_ready_cb);
    s_trans_t0 = lv_tick_get();
    lv_anim_start(&a);
    ESP_LOGI(TAG, "page transition mode=%d dur=%d", v->page_anim, duration);
}

static void tile_press_cb(lv_event_t *e)
{
    (void)e;
    if (!s_view) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_view->pressed = true;
    s_view->press_x = p.x;
    s_view->press_y = p.y;
}

static void tile_release_cb(lv_event_t *e)
{
    (void)e;
    if (!s_view || !s_view->pressed) return;
    s_view->pressed = false;
    /* slide 交给 tileview 原生跟手滚动 */
    if (s_view->page_anim == PAGE_ANIM_SLIDE) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - s_view->press_x;
    int dy = p.y - s_view->press_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx < 50 || adx < ady * 2) return;
    int target = s_view->current_page + (dx < 0 ? 1 : -1);
    if (target < 0 || target >= s_view->page_count) return;
    ESP_LOGI(TAG, "swipe release dx=%d -> page %d", dx, target);
    ui_dynamic_set_page(target);
}

static void free_view(dyn_view_t *v, bool delete_screen)
{
    if (!v) return;
    trans_clear(v);
    if (delete_screen && v->screen) lv_obj_del(v->screen);
    if (v->widgets) heap_caps_free(v->widgets);
    free(v);
}

void ui_dynamic_apply(void)
{
    if (!s_view) return;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < s_view->widget_count; i++) {
        apply_widget(&s_view->widgets[i]);
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
    out->page_last_ms = s_page_last_ms;
    out->page_swaps = s_page_swaps;
    out->trans_last_ms = s_trans_last_ms;
}

static void tile_scroll_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN) {
        s_page_t0 = lv_tick_get();
        return;
    }
    if (code != LV_EVENT_SCROLL_END) return;
    if (s_page_t0) {
        s_page_last_ms = lv_tick_elaps(s_page_t0);
        s_page_t0 = 0;
    }
    s_page_swaps++;
    if (!s_view) return;
    int active = active_page_index(s_view);
    if (active >= 0 && active != s_view->current_page) {
        s_view->current_page = active;
        ESP_LOGI(TAG, "page -> %d (%lld ms)", active,
                 (long long)s_page_last_ms);
    }
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
    ui_dynamic_apply();
}

void ui_dynamic_destroy(void)
{
    if (s_pet_timer) {
        lv_timer_delete(s_pet_timer);
        s_pet_timer = NULL;
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

void ui_dynamic_set_page(int page)
{
    if (!s_view || !s_view->tileview) return;
    if (page < 0) page = 0;
    if (page >= s_view->page_count) page = s_view->page_count - 1;
    if (page == s_view->current_page) return;
    int from = s_view->current_page;

    if (s_view->page_anim == PAGE_ANIM_SLIDE) {
        trans_clear(s_view);
        lv_tileview_set_tile_by_index(s_view->tileview, (uint32_t)page, 0,
                                      LV_ANIM_ON);
        return;
    }

    lv_draw_buf_t *snap = NULL;
    if (s_view->page_anim != PAGE_ANIM_NONE) {
        trans_clear(s_view);
        int64_t t0 = esp_timer_get_time();
        snap = lv_snapshot_take(s_view->screen, LV_COLOR_FORMAT_RGB565);
        ESP_LOGI(TAG, "page snapshot %lld ms (%p)",
                 (long long)((esp_timer_get_time() - t0) / 1000),
                 (void *)snap);
    }

    lv_tileview_set_tile_by_index(s_view->tileview, (uint32_t)page, 0,
                                  LV_ANIM_OFF);
    lv_obj_set_scroll_dir(s_view->tileview, LV_DIR_NONE);
    s_view->current_page = page;
    if (snap) start_transition(s_view, snap, from, page);
}

int ui_dynamic_page_count(void)
{
    return s_view ? s_view->page_count : 0;
}

int ui_dynamic_page_anim(void)
{
    return s_view ? s_view->page_anim : PAGE_ANIM_NONE;
}

void ui_dynamic_set_page_anim(int mode)
{
    if (!s_view) return;
    if (mode < PAGE_ANIM_NONE || mode > PAGE_ANIM_ROTATE) return;
    s_view->page_anim = mode;
    if (mode == PAGE_ANIM_SLIDE) {
        lv_obj_set_scroll_dir(s_view->tileview, LV_DIR_LEFT | LV_DIR_RIGHT);
    } else {
        lv_obj_set_scroll_dir(s_view->tileview, LV_DIR_NONE);
    }
    ESP_LOGI(TAG, "page anim -> %d", mode);
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
        lv_obj_set_size(s_portal_overlay, PROFILE_W, PROFILE_H);
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
        lv_obj_set_size(s_network_overlay, PROFILE_W, PROFILE_H);
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

    const char *profile = jstr(root, "profile", "");
    if (strcmp(profile, "320x480") != 0) {
        ESP_LOGE(TAG, "unsupported profile '%s' (need 320x480)", profile);
        goto out;
    }

    int version = jint(root, "v", 1);
    cJSON *all_pages[4] = {0};
    int page_count = 0;
    const char *page_animation = jstr(root, "page_animation", "slide");

    if (version == 1) {
        all_pages[0] = cJSON_GetObjectItemCaseSensitive(root, "widgets");
        page_count = 1;
    } else if (version == 2) {
        cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) < 1) {
            ESP_LOGE(TAG, "v2 layout has no pages");
            goto out;
        }
        int n = cJSON_GetArraySize(pages);
        page_count = n > 4 ? 4 : n;
        for (int i = 0; i < page_count; i++) {
            cJSON *page = cJSON_GetArrayItem(pages, i);
            if (cJSON_IsObject(page)) {
                all_pages[i] =
                    cJSON_GetObjectItemCaseSensitive(page, "widgets");
            }
        }
    } else {
        ESP_LOGE(TAG, "unsupported layout version %d", version);
        goto out;
    }
    if (page_count == 0) {
        ESP_LOGE(TAG, "layout has no pages");
        goto out;
    }

    char ids[DYN_MAX_SUB_IDS][DASH_METRIC_ID_LEN];
    memset(ids, 0, sizeof(ids));
    int id_count = 0;
    for (int i = 0; i < page_count; i++) {
        if (!cJSON_IsArray(all_pages[i])) continue;
        int n = cJSON_GetArraySize(all_pages[i]);
        for (int j = 0; j < n; j++) {
            cJSON *wj = cJSON_GetArrayItem(all_pages[i], j);
            if (cJSON_IsObject(wj)) collect_widget_ids(wj, ids, &id_count);
        }
    }
    build_subscribe(ids, id_count, subscribe_out, subscribe_cap);

    next->widgets = heap_caps_calloc(DYN_MAX_WIDGETS, sizeof(dyn_widget_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next->widgets) {
        ESP_LOGE(TAG, "no PSRAM for dynamic widgets");
        goto out;
    }

    for (int pi = 0; pi < page_count; pi++) {
        if (!cJSON_IsArray(all_pages[pi])) continue;
        int n = cJSON_GetArraySize(all_pages[pi]);
        for (int i = 0; i < n && next->widget_count < DYN_MAX_WIDGETS; i++) {
            cJSON *wj = cJSON_GetArrayItem(all_pages[pi], i);
            if (!cJSON_IsObject(wj)) continue;
            dyn_widget_t parsed;
            if (!parse_widget(wj, &parsed)) {
                ESP_LOGW(TAG, "skip unsupported widget at page %d index %d",
                         pi, i);
                continue;
            }
            parsed.page = pi;
            next->widgets[next->widget_count++] = parsed;
        }
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
    lv_obj_remove_flag(next->screen, LV_OBJ_FLAG_SCROLLABLE);

    next->page_count = page_count;
    next->page_anim = (strcmp(page_animation, "none") == 0) ? PAGE_ANIM_NONE
                      : (strcmp(page_animation, "fade") == 0) ? PAGE_ANIM_FADE
                      : (strcmp(page_animation, "stack") == 0) ? PAGE_ANIM_STACK
                      : (strcmp(page_animation, "zoom") == 0) ? PAGE_ANIM_ZOOM
                      : (strcmp(page_animation, "flip") == 0) ? PAGE_ANIM_FLIP
                      : (strcmp(page_animation, "rotate") == 0) ? PAGE_ANIM_ROTATE
                                                              : PAGE_ANIM_SLIDE;
    next->current_page = 0;
    next->pressed = false;
    next->tileview = lv_tileview_create(next->screen);
    lv_obj_set_pos(next->tileview, 0, 0);
    lv_obj_set_size(next->tileview, PROFILE_W, PROFILE_H);
    lv_obj_set_style_bg_opa(next->tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(next->tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(next->tileview, tile_scroll_cb, LV_EVENT_SCROLL_BEGIN,
                        NULL);
    lv_obj_add_event_cb(next->tileview, tile_scroll_cb, LV_EVENT_SCROLL_END,
                        NULL);
    lv_obj_add_event_cb(next->screen, tile_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(next->screen, tile_release_cb, LV_EVENT_RELEASED, NULL);
    for (int i = 0; i < next->page_count; i++) {
        next->tiles[i] = lv_tileview_add_tile(next->tileview, (uint8_t)i, 0,
                                              LV_DIR_LEFT | LV_DIR_RIGHT);
        if (next->tiles[i]) {
            lv_obj_set_style_bg_opa(next->tiles[i], LV_OPA_TRANSP, 0);
        }
    }
    for (int i = 0; i < next->widget_count; i++) {
        int page = next->widgets[i].page;
        if (page < 0 || page >= next->page_count) page = 0;
        build_widget_ui(&next->widgets[i],
                        next->tiles[page] ? next->tiles[page] : next->screen);
    }
    enable_gesture_bubble(next->tileview);
    if (next->page_anim != PAGE_ANIM_SLIDE) {
        lv_obj_set_scroll_dir(next->tileview, LV_DIR_NONE);
    }

    s_staging = next;
    ok = true;
    ESP_LOGI(TAG,
             "dynamic UI staged: %d pages, %d widgets, %d subscribed metrics,"
             " anim=%s",
             next->page_count, next->widget_count, id_count, page_animation);

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
        if (strcmp(type, "status_pill") == 0 || strcmp(type, "kpi") == 0 ||
            strcmp(type, "sparkline") == 0 ||
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
