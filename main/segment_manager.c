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

/* In-RAM segment state */
static segment_geom_t  s_geom[MAX_SEGMENTS];
static segment_light_t s_state[MAX_SEGMENTS];

void segment_manager_init(void)
{
    memset(s_geom, 0, sizeof(s_geom));
    memset(s_state, 0, sizeof(s_state));

    /* Default light state: off, 50% brightness, hue 0 */
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        s_state[i].level = 128;
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

    size_t sz = sizeof(s_geom);
    err = nvs_get_blob(h, NVS_KEY_GEOM, s_geom, &sz);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Segment geometry loaded");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "seg_geom load error: %s", esp_err_to_name(err));
    }

    sz = sizeof(s_state);
    err = nvs_get_blob(h, NVS_KEY_STATE, s_state, &sz);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Segment state loaded");
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
