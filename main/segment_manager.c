/**
 * @file segment_manager.c
 * @brief Virtual segment state and NVS persistence
 */

#include "segment_manager.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "seg_mgr";

#define NVS_NAMESPACE   "led_cfg"
#define NVS_KEY_GEOM    "seg_geom"
#define NVS_KEY_STATE   "seg_state"

static segment_geom_t  s_geom[MAX_SEGMENTS];
static segment_light_t s_state[MAX_SEGMENTS];

/**
 * @brief On-disk representation of segment_light_t (fields only, no runtime transition state).
 *
 * The in-memory segment_light_t embeds transition_t fields which must NOT be
 * persisted.  All NVS blobs use this smaller struct so that the on-disk size
 * remains stable across firmware upgrades that change transition_t internals.
 */
typedef struct {
    bool     on;
    uint8_t  level;
    uint16_t hue;
    uint8_t  saturation;
    uint8_t  color_mode;
    uint16_t color_temp;
    uint8_t  startup_on_off;
} segment_light_nvs_t;

void segment_manager_init(uint16_t default_count)
{
    memset(s_geom, 0, sizeof(s_geom));
    memset(s_state, 0, sizeof(s_state));

    /* Segment 1 (index 0) covers the full strip by default */
    s_geom[0].start = 0;
    s_geom[0].count = default_count;

    /* Default light state: off, 50% brightness, restore previous on reboot */
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        s_state[i].level = 128;
        s_state[i].color_temp = 250;     /* ~4000K neutral */
        s_state[i].startup_on_off = DEFAULT_STARTUP_ON_OFF;
    }
}

segment_geom_t *segment_geom_get(void)
{
    return s_geom;
}

segment_light_t *segment_state_get(void)
{
    return s_state;
}

/**
 * @brief Initialise transition current_values from the in-memory state.
 *
 * Must be called after segment_manager_load() so that the transition engine
 * starts from the correct value rather than 0.  Does NOT call
 * transition_register() — that is done in zigbee_handlers.c after the
 * transition engine itself has been initialised.
 */
void segment_manager_init_transitions(void)
{
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        s_state[i].level_trans.current_value = s_state[i].level;
        s_state[i].hue_trans.current_value   = s_state[i].hue;
        s_state[i].sat_trans.current_value   = s_state[i].saturation;
        s_state[i].ct_trans.current_value    = s_state[i].color_temp;
    }
}

void segment_manager_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS for segment load: %s", esp_err_to_name(err));
        return;
    }

    /* Load geometry into temp buffer to detect struct size mismatch.
     * The struct gained strip_id in Phase 4; old NVS blobs are smaller. */
    uint8_t geom_tmp[sizeof(s_geom)];
    size_t sz = sizeof(geom_tmp);
    err = nvs_get_blob(h, NVS_KEY_GEOM, geom_tmp, &sz);
    if (err == ESP_OK) {
        if (sz == sizeof(s_geom)) {
            memcpy(s_geom, geom_tmp, sizeof(s_geom));
            ESP_LOGI(TAG, "Segment geometry loaded");
        } else {
            ESP_LOGW(TAG, "Segment geometry format changed (stored=%zu expected=%zu), using defaults",
                     sz, sizeof(s_geom));
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "seg_geom load error: %s", esp_err_to_name(err));
    }

    /* Load state into NVS structs (never includes transition_t runtime fields).
     * Version history:
     *   v1: 12 bytes/entry (no startup_on_off)
     *   v2: sizeof(segment_light_nvs_t) per entry (added startup_on_off)
     *   v3+: if on-disk == sizeof(old segment_light_t with no transition_t),
     *        treat as v2 (backwards-compatible upgrade from pre-transition builds).
     */
#define SEGMENT_STATE_V1_SIZE  12   /* sizeof old segment_light_t, no startup_on_off */
    segment_light_nvs_t nvs_state[MAX_SEGMENTS];
    sz = sizeof(nvs_state);
    err = nvs_get_blob(h, NVS_KEY_STATE, nvs_state, &sz);
    if (err == ESP_OK) {
        if (sz == sizeof(nvs_state)) {
            /* Current format: copy all fields from persist struct */
            for (int i = 0; i < MAX_SEGMENTS; i++) {
                s_state[i].on           = nvs_state[i].on;
                s_state[i].level        = nvs_state[i].level;
                s_state[i].hue          = nvs_state[i].hue;
                s_state[i].saturation   = nvs_state[i].saturation;
                s_state[i].color_mode   = nvs_state[i].color_mode;
                s_state[i].color_temp   = nvs_state[i].color_temp;
                s_state[i].startup_on_off = nvs_state[i].startup_on_off;
            }
            ESP_LOGI(TAG, "Segment state loaded");
        } else if (sz == MAX_SEGMENTS * SEGMENT_STATE_V1_SIZE) {
            /* v1 format: no startup_on_off, read fields manually */
            uint8_t *p = (uint8_t *)nvs_state;
            for (int i = 0; i < MAX_SEGMENTS; i++) {
                memcpy(&s_state[i], p + i * SEGMENT_STATE_V1_SIZE,
                       SEGMENT_STATE_V1_SIZE);
                s_state[i].startup_on_off = DEFAULT_STARTUP_ON_OFF;
            }
            ESP_LOGI(TAG, "Segment state migrated (v1 -> v2)");
        } else {
            ESP_LOGW(TAG, "Segment state format unrecognized (sz=%zu), using defaults", sz);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "seg_state load error: %s", esp_err_to_name(err));
    }

    nvs_close(h);
}

void segment_manager_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS for segment save: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, NVS_KEY_GEOM, s_geom, sizeof(s_geom));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seg_geom save failed: %s", esp_err_to_name(err));
    }

    /* Build NVS-only structs (no transition_t runtime fields) before saving */
    segment_light_nvs_t nvs_state[MAX_SEGMENTS];
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        nvs_state[i].on           = s_state[i].on;
        nvs_state[i].level        = s_state[i].level;
        nvs_state[i].hue          = s_state[i].hue;
        nvs_state[i].saturation   = s_state[i].saturation;
        nvs_state[i].color_mode   = s_state[i].color_mode;
        nvs_state[i].color_temp   = s_state[i].color_temp;
        nvs_state[i].startup_on_off = s_state[i].startup_on_off;
    }
    err = nvs_set_blob(h, NVS_KEY_STATE, nvs_state, sizeof(nvs_state));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seg_state save failed: %s", esp_err_to_name(err));
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(h);
}
