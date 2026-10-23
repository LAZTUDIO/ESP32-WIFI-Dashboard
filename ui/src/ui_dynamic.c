#include "ui_dynamic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "dash_metric_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "uidyn";

#define DYN_MAX_WIDGETS 20
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
    lv_obj_t *kpi_label;
    lv_obj_t *server_label[4];
    lv_obj_t *server_value[4];
    lv_obj_t *list_dot[DYN_MAX_ROWS];
    lv_obj_t *list_label[DYN_MAX_ROWS];
    lv_obj_t *list_value[DYN_MAX_ROWS];
} dyn_widget_t;

static lv_obj_t *s_screen;
static dyn_widget_t *s_widgets;
static int s_widget_count;
static bool s_active;

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

    if (w->type == DYN_KPI || w->type == DYN_SPARK) {
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

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *row = ui_group_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
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
    lv_obj_set_style_bg_color(dot, lv_color_hex(UI_TEXT_MUT), 0);
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
    w->status_model = make_label(w->body, "—", UI_F_CJK, UI_TEXT);
    lv_obj_set_flex_grow(w->status_model, 1);
    lv_obj_set_style_max_width(w->status_model, LV_PCT(70), 0);
    lv_obj_set_height(w->status_model, 18);
    lv_label_set_long_mode(w->status_model, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *pill = ui_pill_create(w->body, "—", UI_ACCENT);
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
    w->kpi_value = make_label(row, "--", value_font, UI_TEXT);
    lv_label_set_long_mode(w->kpi_value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(w->kpi_value, LV_PCT(72), 0);
    w->kpi_unit = make_label(row, w->value_unit, UI_F_CAP, UI_TEXT_MUT);

    w->kpi_label = make_label(w->body, w->value_label, UI_F_CJK, UI_TEXT_MUT);
    lv_obj_set_width(w->kpi_label, LV_PCT(100));
    lv_label_set_long_mode(w->kpi_label, LV_LABEL_LONG_MODE_DOTS);
}

static void build_server(dyn_widget_t *w)
{
    const int body_w = w->body_w;
    const int body_h = w->body_h;
    int row_h = body_h / 4;
    if (row_h < 16) row_h = 16;
    const int label_w = body_w * 45 / 100;
    const int value_w = body_w - label_w;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = ui_group_create(w->body);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, body_w, row_h);

        w->server_label[i] = make_label(row, w->server[i].label, UI_F_CJK,
                                        UI_TEXT_MUT);
        lv_obj_set_pos(w->server_label[i], 0, 0);
        lv_obj_set_size(w->server_label[i], label_w, row_h);
        lv_label_set_long_mode(w->server_label[i], LV_LABEL_LONG_MODE_DOTS);

        w->server_value[i] = make_label(row, "—", UI_F_CAP, UI_TEXT);
        lv_obj_set_pos(w->server_value[i], label_w, 0);
        lv_obj_set_size(w->server_value[i], value_w, row_h);
        lv_obj_set_style_text_align(w->server_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(w->server_value[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_metric_list(dyn_widget_t *w)
{
    const int body_w = w->body_w;
    const int body_h = w->body_h;
    const int count = w->row_count > 0 ? w->row_count : 1;
    int row_h = body_h / count;
    if (row_h < 14) row_h = 14;

    const lv_font_t *value_font = (w->list_font_size == 0) ? UI_F_CAP
                                   : (w->list_font_size == 2) ? UI_F_H3
                                                              : UI_F_BODY;
    for (int i = 0; i < w->row_count; i++) {
        dyn_row_t *r = &w->rows[i];
        lv_obj_t *row = ui_group_create(w->body);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, body_w, row_h);

        int x = 0;
        bool show_dot = (strcmp(r->mtype, "bool") == 0) ||
                        (strcmp(r->kind, "status") == 0);
        if (show_dot) {
            w->list_dot[i] = make_dot(row, 7);
            lv_obj_set_pos(w->list_dot[i], 0, (row_h - 7) / 2);
            x += 9;
        }
        int label_w = w->list_show_labels ? body_w * 38 / 100 : 0;
        if (w->list_show_labels) {
            w->list_label[i] = make_label(row, r->label, UI_F_CJK, UI_TEXT_MUT);
            lv_obj_set_pos(w->list_label[i], x, 0);
            lv_obj_set_size(w->list_label[i], label_w, row_h);
            lv_label_set_long_mode(w->list_label[i], LV_LABEL_LONG_MODE_DOTS);
            x += label_w;
        }
        w->list_value[i] = make_label(row, "—", value_font, UI_TEXT);
        lv_obj_set_pos(w->list_value[i], x, 0);
        lv_obj_set_size(w->list_value[i], body_w - x, row_h);
        lv_obj_set_style_text_align(w->list_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(w->list_value[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_widget_ui(dyn_widget_t *w)
{
    int x, y, ww, hh;
    grid_rect(w->x, w->y, w->w, w->h, &x, &y, &ww, &hh);

    lv_obj_t *card = lv_obj_create(s_screen);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_R_M, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, ww, hh);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    w->root = card;

    int header_h = 0;
    if (w->show_title) {
        lv_obj_t *title = ui_caption_create(card, w->title);
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
    default:
        break;
    }
}

/* ---------------- value formatting ---------------- */

static void format_metric(char *out, size_t cap, const dash_metric_t *m,
                          int precision, const char *unit)
{
    if (!m || !m->available) {
        snprintf(out, cap, "—");
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
        snprintf(out, cap, "%s", m->text[0] ? m->text : "—");
    }
}

static void format_number_only(char *out, size_t cap, const dash_metric_t *m,
                               int precision)
{
    if (!m || !m->available) {
        snprintf(out, cap, "—");
        return;
    }
    if (m->type == DASH_METRIC_NUMBER) {
        if (precision < 0) precision = 0;
        if (precision > 3) precision = 3;
        snprintf(out, cap, "%.*f", precision, m->number);
    } else if (m->type == DASH_METRIC_BOOL) {
        snprintf(out, cap, "%s", m->boolean ? "是" : "否");
    } else {
        snprintf(out, cap, "%s", m->text[0] ? m->text : "—");
    }
}

static void set_dot(lv_obj_t *dot, const dash_metric_t *m)
{
    if (!dot) return;
    uint32_t color = UI_TEXT_MUT;
    if (m && m->available && m->type == DASH_METRIC_BOOL) {
        color = m->boolean ? UI_OK : UI_ERR;
    }
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
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
            lv_label_set_text(w->status_model, text[0] ? text : "—");
        }
        if (w->status_phase) {
            const char *text = (w->phase_metric && w->phase_metric->available)
                                   ? w->phase_metric->text
                                   : "未知";
            lv_label_set_text(w->status_phase, text[0] ? text : "未知");
        }
        break;

    case DYN_KPI:
    case DYN_SPARK: {
        format_number_only(buf, sizeof(buf), w->value_metric,
                           w->value_precision);
        if (w->kpi_value) lv_label_set_text(w->kpi_value, buf);
        if (w->kpi_unit) lv_label_set_text(w->kpi_unit, w->value_unit);
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
                    text = "—";
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
                text = (m && m->available) ? m->text : "—";
            } else {
                format_metric(buf, sizeof(buf), w->server[i].metric,
                              w->server[i].precision, w->server[i].unit);
                text = buf;
            }
            if (w->server_value[i] && text) {
                lv_label_set_text(w->server_value[i], text);
            }
        }
        break;
    }

    case DYN_LIST: {
        for (int i = 0; i < w->row_count; i++) {
            dyn_row_t *r = &w->rows[i];
            const dash_metric_t *m = r->metric;
            const char *text;
            if (!m || !m->available) {
                text = "—";
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
                    lv_label_set_text(w->list_value[i], formatted);
                }
            } else if (w->list_value[i]) {
                lv_label_set_text(w->list_value[i], text);
            }
            if (w->list_dot[i]) set_dot(w->list_dot[i], m);
        }
        break;
    }

    default:
        break;
    }
}

void ui_dynamic_apply(void)
{
    if (!s_active) return;
    for (int i = 0; i < s_widget_count; i++) {
        apply_widget(&s_widgets[i]);
    }
}

bool ui_dynamic_active(void)
{
    return s_active;
}

void ui_dynamic_destroy(void)
{
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }
    s_active = false;
    if (s_widgets) {
        heap_caps_free(s_widgets);
        s_widgets = NULL;
    }
    s_widget_count = 0;
}

/* ---------------- public build ---------------- */

bool ui_dynamic_build(const char *layout_json, char *subscribe_out,
                      size_t subscribe_cap)
{
    if (!layout_json || !subscribe_out || subscribe_cap < 32) return false;
    if (s_active) return true;

    cJSON *root = cJSON_Parse(layout_json);
    if (!root) {
        ESP_LOGE(TAG, "layout JSON parse failed");
        return false;
    }

    const char *profile = jstr(root, "profile", "");
    if (strcmp(profile, "320x480") != 0) {
        ESP_LOGE(TAG, "unsupported profile '%s' (need 320x480)", profile);
        cJSON_Delete(root);
        return false;
    }

    int version = jint(root, "v", 1);
    cJSON *pages = NULL;
    cJSON *render_widgets = NULL;
    cJSON *all_pages[4] = {0};
    int all_count = 0;

    if (version == 1) {
        render_widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
        all_pages[0] = render_widgets;
        all_count = 1;
    } else if (version == 2) {
        pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) < 1) {
            ESP_LOGE(TAG, "v2 layout has no pages");
            cJSON_Delete(root);
            return false;
        }
        cJSON *page0 = cJSON_GetArrayItem(pages, 0);
        render_widgets = cJSON_IsObject(page0)
                             ? cJSON_GetObjectItemCaseSensitive(page0, "widgets")
                             : NULL;
        int n = cJSON_GetArraySize(pages);
        for (int i = 0; i < n && all_count < 4; i++) {
            cJSON *page = cJSON_GetArrayItem(pages, i);
            if (cJSON_IsObject(page)) {
                all_pages[all_count++] =
                    cJSON_GetObjectItemCaseSensitive(page, "widgets");
            }
        }
    } else {
        ESP_LOGE(TAG, "unsupported layout version %d", version);
        cJSON_Delete(root);
        return false;
    }

    char ids[DYN_MAX_SUB_IDS][DASH_METRIC_ID_LEN];
    memset(ids, 0, sizeof(ids));
    int id_count = 0;
    for (int i = 0; i < all_count; i++) {
        if (!cJSON_IsArray(all_pages[i])) continue;
        int n = cJSON_GetArraySize(all_pages[i]);
        for (int j = 0; j < n; j++) {
            cJSON *wj = cJSON_GetArrayItem(all_pages[i], j);
            if (cJSON_IsObject(wj)) collect_widget_ids(wj, ids, &id_count);
        }
    }
    build_subscribe(ids, id_count, subscribe_out, subscribe_cap);

    if (!cJSON_IsArray(render_widgets)) {
        ESP_LOGE(TAG, "layout has no renderable widgets");
        cJSON_Delete(root);
        return false;
    }

    s_widgets = heap_caps_calloc(DYN_MAX_WIDGETS, sizeof(dyn_widget_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_widgets) {
        ESP_LOGE(TAG, "no PSRAM for dynamic widgets");
        cJSON_Delete(root);
        return false;
    }
    s_widget_count = 0;

    int n = cJSON_GetArraySize(render_widgets);
    for (int i = 0; i < n && s_widget_count < DYN_MAX_WIDGETS; i++) {
        cJSON *wj = cJSON_GetArrayItem(render_widgets, i);
        if (!cJSON_IsObject(wj)) continue;
        dyn_widget_t parsed;
        if (!parse_widget(wj, &parsed)) {
            ESP_LOGW(TAG, "skip unsupported widget at index %d", i);
            continue;
        }
        s_widgets[s_widget_count++] = parsed;
    }

    if (s_widget_count == 0) {
        ESP_LOGE(TAG, "no supported widgets");
        heap_caps_free(s_widgets);
        s_widgets = NULL;
        cJSON_Delete(root);
        return false;
    }

    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        ESP_LOGE(TAG, "screen create failed");
        heap_caps_free(s_widgets);
        s_widgets = NULL;
        cJSON_Delete(root);
        return false;
    }
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < s_widget_count; i++) {
        build_widget_ui(&s_widgets[i]);
    }

    lv_screen_load(s_screen);
    s_active = true;
    ui_dynamic_apply();

    ESP_LOGI(TAG, "dynamic UI: %d widgets, %d subscribed metrics",
             s_widget_count, id_count);
    cJSON_Delete(root);
    return true;
}
