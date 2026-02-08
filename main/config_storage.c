/**
 * @file config_storage.c
 * @brief NVS persistence for device configuration (LED strip count)
 */

#include "config_storage.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config";

#define NVS_NAMESPACE       "led_cfg"
#define NVS_KEY_LED_COUNT   "led_cnt"

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

esp_err_t config_storage_save_led_count(uint16_t count)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, NVS_KEY_LED_COUNT, count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGE(TAG, "Save led_count failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t config_storage_load_led_count(uint16_t *count)
{
    if (!count) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_u16(h, NVS_KEY_LED_COUNT, count);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    return err;
}
