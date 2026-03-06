/**
 * @file config_storage.c
 * @brief NVS persistence for device configuration (per-strip LED counts)
 *
 * Keys in "led_cfg" namespace:
 *   "led_cnt_1" - uint16, strip 0 LED count
 *   "led_cnt_2" - uint16, strip 1 LED count
 *   "led_cnt"   - legacy key for strip 0 (migration fallback)
 */

#include "config_storage.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config";

#define NVS_NAMESPACE    "led_cfg"

static const char *s_keys[2] = {"led_cnt_1", "led_cnt_2"};
static const char *s_type_keys[2]    = {"strip_typ_1", "strip_typ_2"};
static const char *s_cur_keys[2]     = {"max_cur_1", "max_cur_2"};

esp_err_t config_storage_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "NVS namespace '%s' ready", NVS_NAMESPACE);
    return ESP_OK;
}

esp_err_t config_storage_save_strip_count(uint8_t strip, uint16_t count)
{
    if (strip >= 2) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, s_keys[strip], count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGE(TAG, "Save strip%d count failed: %s", strip, esp_err_to_name(err));
    return err;
}

esp_err_t config_storage_load_strip_count(uint8_t strip, uint16_t *count)
{
    if (strip >= 2 || !count) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_u16(h, s_keys[strip], count);
    /* Migration: fall back to legacy "led_cnt" for strip 0 */
    if (err == ESP_ERR_NVS_NOT_FOUND && strip == 0) {
        err = nvs_get_u16(h, "led_cnt", count);
    }
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    return err;
}

esp_err_t config_storage_save_strip_type(uint8_t strip, uint8_t type)
{
    if (strip >= 2) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(h, s_type_keys[strip], type);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGE(TAG, "Save strip%d type failed: %s", strip, esp_err_to_name(err));
    return err;
}

esp_err_t config_storage_load_strip_type(uint8_t strip, uint8_t *type)
{
    if (strip >= 2 || !type) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_u8(h, s_type_keys[strip], type);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    return err;
}

esp_err_t config_storage_save_strip_max_current(uint8_t strip, uint16_t ma)
{
    if (strip >= 2) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, s_cur_keys[strip], ma);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGE(TAG, "Save strip%d max_current failed: %s", strip, esp_err_to_name(err));
    return err;
}

esp_err_t config_storage_load_strip_max_current(uint8_t strip, uint16_t *ma)
{
    if (strip >= 2 || !ma) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_u16(h, s_cur_keys[strip], ma);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    return err;
}

esp_err_t config_storage_save_global_transition_ms(uint16_t ms)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, "glob_trans", ms);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGE(TAG, "Save global_transition_ms failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t config_storage_load_global_transition_ms(uint16_t *ms)
{
    if (!ms) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_u16(h, "glob_trans", ms);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    return err;
}
