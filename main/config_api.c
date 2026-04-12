// SPDX-License-Identifier: MIT
/**
 * config_api.c — Web API bridge to LED segment/preset/strip state (C6 only).
 *
 * Reads from and writes to the same state that the Zigbee attribute handler uses,
 * so changes propagate immediately to the 200Hz render loop.
 */
#include "config_api.h"

#include "board_config.h"
#include "config_storage.h"
#include "led_renderer.h"
#include "preset_manager.h"
#include "segment_manager.h"
#include "transition_engine.h"

#include "cJSON.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "config_api";

/* ========================================================================== */
/*  Strip config                                                               */
/* ========================================================================== */

extern uint16_t g_strip_count[2];
extern uint8_t  g_strip_type[2];
extern uint16_t g_strip_max_current[2];

esp_err_t config_api_get_strip_config(cJSON **out)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(j, "strip1_count",       g_strip_count[0]);
    cJSON_AddNumberToObject(j, "strip2_count",       g_strip_count[1]);
    cJSON_AddNumberToObject(j, "strip1_type",        g_strip_type[0]);
    cJSON_AddNumberToObject(j, "strip2_type",        g_strip_type[1]);
    cJSON_AddNumberToObject(j, "strip1_max_current", g_strip_max_current[0]);
    cJSON_AddNumberToObject(j, "strip2_max_current", g_strip_max_current[1]);
    cJSON_AddNumberToObject(j, "transition_ms",      led_renderer_get_global_transition_ms());

    *out = j;
    return ESP_OK;
}

esp_err_t config_api_set_strip_config(const cJSON *obj)
{
    if (!obj) return ESP_ERR_INVALID_ARG;

    bool dirty = false;

    cJSON *item;

#define APPLY_STRIP_U16(key, field) \
    if ((item = cJSON_GetObjectItem(obj, key)) && cJSON_IsNumber(item)) { \
        (field) = (uint16_t)item->valueint; dirty = true; \
    }

    APPLY_STRIP_U16("strip1_count",       g_strip_count[0]);
    APPLY_STRIP_U16("strip2_count",       g_strip_count[1]);
    APPLY_STRIP_U16("strip1_max_current", g_strip_max_current[0]);
    APPLY_STRIP_U16("strip2_max_current", g_strip_max_current[1]);

#undef APPLY_STRIP_U16

    if ((item = cJSON_GetObjectItem(obj, "strip1_type")) && cJSON_IsNumber(item)) {
        g_strip_type[0] = (uint8_t)item->valueint;
        config_storage_save_strip_type(0, g_strip_type[0]);
        dirty = true;
    }
    if ((item = cJSON_GetObjectItem(obj, "strip2_type")) && cJSON_IsNumber(item)) {
        g_strip_type[1] = (uint8_t)item->valueint;
        config_storage_save_strip_type(1, g_strip_type[1]);
        dirty = true;
    }
    if ((item = cJSON_GetObjectItem(obj, "transition_ms")) && cJSON_IsNumber(item)) {
        uint16_t ms = (uint16_t)item->valueint;
        led_renderer_set_global_transition_ms(ms);
        config_storage_save_global_transition_ms(ms);
        dirty = true;
    }

    if (dirty) {
        /* Persist strip counts and currents */
        for (int i = 0; i < 2; i++) {
            config_storage_save_strip_count(i, g_strip_count[i]);
            config_storage_save_strip_max_current(i, g_strip_max_current[i]);
        }
        led_renderer_recalc_power_scale();
    }

    return ESP_OK;
}

/* ========================================================================== */
/*  Segment serialisation helpers                                              */
/* ========================================================================== */

static const char *startup_str(uint8_t v)
{
    switch (v) {
    case 0x00: return "off";
    case 0x01: return "on";
    case 0x02: return "toggle";
    default:   return "previous";
    }
}

static uint8_t startup_from_str(const char *s)
{
    if (!s)                      return 0xFF;
    if (!strcmp(s, "off"))       return 0x00;
    if (!strcmp(s, "on"))        return 0x01;
    if (!strcmp(s, "toggle"))    return 0x02;
    return 0xFF;
}

static cJSON *segment_to_json(int idx)
{
    segment_light_t *state = segment_state_get();
    segment_geom_t  *geom  = segment_geom_get();
    const segment_light_t *s = &state[idx];
    const segment_geom_t  *g = &geom[idx];

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "index",   idx);
    cJSON_AddBoolToObject  (j, "on",      s->on);
    cJSON_AddNumberToObject(j, "level",   s->level);
    cJSON_AddStringToObject(j, "mode",    s->color_mode == 2 ? "ct" : "hs");
    cJSON_AddNumberToObject(j, "hue",     s->hue);
    cJSON_AddNumberToObject(j, "sat",     s->saturation);
    cJSON_AddNumberToObject(j, "ct",      s->color_temp);
    cJSON_AddNumberToObject(j, "start",   g->start);
    cJSON_AddNumberToObject(j, "count",   g->count);
    cJSON_AddNumberToObject(j, "strip",   g->strip_id);
    cJSON_AddStringToObject(j, "startup", startup_str(s->startup_on_off));
    return j;
}

static void apply_segment_json(int idx, const cJSON *obj)
{
    segment_light_t *state = segment_state_get();
    segment_geom_t  *geom  = segment_geom_get();
    segment_light_t *s = &state[idx];
    segment_geom_t  *g = &geom[idx];

    cJSON *item;
    uint32_t trans_ms = led_renderer_get_global_transition_ms();

    if ((item = cJSON_GetObjectItem(obj, "on")) && cJSON_IsBool(item)) {
        s->on = cJSON_IsTrue(item);
        if (s->on) {
            transition_start(&s->level_trans, s->level, trans_ms);
        } else {
            transition_start(&s->level_trans, 0, trans_ms);
        }
    }
    if ((item = cJSON_GetObjectItem(obj, "level")) && cJSON_IsNumber(item)) {
        s->level = (uint8_t)item->valueint;
        if (s->on) transition_start(&s->level_trans, s->level, trans_ms);
    }
    if ((item = cJSON_GetObjectItem(obj, "mode")) && cJSON_IsString(item)) {
        s->color_mode = strcmp(item->valuestring, "ct") == 0 ? 2 : 0;
    }
    if ((item = cJSON_GetObjectItem(obj, "hue")) && cJSON_IsNumber(item)) {
        s->hue = (uint16_t)item->valueint;
        transition_start(&s->hue_trans, s->hue, trans_ms);
    }
    if ((item = cJSON_GetObjectItem(obj, "sat")) && cJSON_IsNumber(item)) {
        s->saturation = (uint8_t)item->valueint;
        transition_start(&s->sat_trans, s->saturation, trans_ms);
    }
    if ((item = cJSON_GetObjectItem(obj, "ct")) && cJSON_IsNumber(item)) {
        s->color_temp = (uint16_t)item->valueint;
        transition_start(&s->ct_trans, s->color_temp, trans_ms);
    }
    if ((item = cJSON_GetObjectItem(obj, "startup")) && cJSON_IsString(item)) {
        s->startup_on_off = startup_from_str(item->valuestring);
    }
    /* Geometry: only if explicitly provided */
    if ((item = cJSON_GetObjectItem(obj, "start")) && cJSON_IsNumber(item))
        g->start = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(obj, "count")) && cJSON_IsNumber(item))
        g->count = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(obj, "strip")) && cJSON_IsNumber(item))
        g->strip_id = (uint8_t)item->valueint;

    segment_manager_save();
}

/* ========================================================================== */
/*  Segments                                                                   */
/* ========================================================================== */

esp_err_t config_api_get_segments(cJSON **out)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *arr = cJSON_AddArrayToObject(root, "segments");
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        cJSON_AddItemToArray(arr, segment_to_json(i));
    }
    *out = root;
    return ESP_OK;
}

esp_err_t config_api_set_segments(const cJSON *obj)
{
    if (!obj) return ESP_ERR_INVALID_ARG;

    /* Multi-segment: { "segments": [...] } */
    cJSON *arr = cJSON_GetObjectItem(obj, "segments");
    if (cJSON_IsArray(arr)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, arr) {
            cJSON *idx_item = cJSON_GetObjectItem(entry, "index");
            if (!idx_item || !cJSON_IsNumber(idx_item)) continue;
            int idx = idx_item->valueint;
            if (idx < 0 || idx >= MAX_SEGMENTS) continue;
            apply_segment_json(idx, entry);
        }
        return ESP_OK;
    }

    /* Single-segment: { "index": N, ...fields } */
    cJSON *idx_item = cJSON_GetObjectItem(obj, "index");
    if (idx_item && cJSON_IsNumber(idx_item)) {
        int idx = idx_item->valueint;
        if (idx < 0 || idx >= MAX_SEGMENTS) return ESP_ERR_INVALID_ARG;
        apply_segment_json(idx, obj);
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

/* ========================================================================== */
/*  Presets                                                                    */
/* ========================================================================== */

esp_err_t config_api_get_presets(cJSON **out)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *arr = cJSON_AddArrayToObject(root, "presets");

    for (int i = 0; i < MAX_PRESET_SLOTS; i++) {
        bool occupied = false;
        preset_manager_is_slot_occupied(i, &occupied);
        char name[PRESET_NAME_MAX + 1] = "";
        if (occupied) preset_manager_get_slot_name(i, name, sizeof(name));

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "slot",     i);
        cJSON_AddBoolToObject  (entry, "occupied", occupied);
        cJSON_AddStringToObject(entry, "name",     name);
        cJSON_AddItemToArray(arr, entry);
    }

    *out = root;
    return ESP_OK;
}

esp_err_t config_api_apply_preset(int slot)
{
    if (slot < 0 || slot >= MAX_PRESET_SLOTS) return ESP_ERR_INVALID_ARG;
    esp_err_t err = preset_manager_recall(slot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply_preset slot %d: %s", slot, esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_api_save_preset(int slot, const char *name)
{
    if (slot < 0 || slot >= MAX_PRESET_SLOTS) return ESP_ERR_INVALID_ARG;
    return preset_manager_save(slot, (name && name[0]) ? name : NULL);
}

esp_err_t config_api_delete_preset(int slot)
{
    if (slot < 0 || slot >= MAX_PRESET_SLOTS) return ESP_ERR_INVALID_ARG;
    return preset_manager_delete(slot);
}
