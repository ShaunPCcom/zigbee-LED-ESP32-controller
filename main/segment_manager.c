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

    /* Load state into temp buffer to detect struct size mismatch.
     * The struct gained startup_on_off; old blobs are 12 bytes/entry. */
#define SEGMENT_STATE_V1_SIZE  12   /* sizeof old segment_light_t */
    uint8_t state_tmp[sizeof(s_state)];
    sz = sizeof(state_tmp);
    err = nvs_get_blob(h, NVS_KEY_STATE, state_tmp, &sz);
    if (err == ESP_OK) {
        if (sz == sizeof(s_state)) {
            memcpy(s_state, state_tmp, sizeof(s_state));
            ESP_LOGI(TAG, "Segment state loaded");
        } else if (sz == MAX_SEGMENTS * SEGMENT_STATE_V1_SIZE) {
            for (int i = 0; i < MAX_SEGMENTS; i++) {
                memcpy(&s_state[i], state_tmp + i * SEGMENT_STATE_V1_SIZE,
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

    err = nvs_set_blob(h, NVS_KEY_STATE, s_state, sizeof(s_state));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seg_state save failed: %s", esp_err_to_name(err));
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(h);
}
