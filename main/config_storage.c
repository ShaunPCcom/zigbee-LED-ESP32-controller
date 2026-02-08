/**
 * @file config_storage.c
 * @brief NVS-backed persistence for LED state
 */

#include "config_storage.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config";

#define NVS_NAMESPACE       "led_cfg"
#define NVS_KEY_STATE       "led_state"
#define NVS_KEY_LED_COUNT   "led_cnt"

esp_err_t config_storage_init(void)
{
    /* Verify we can open the namespace */
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

esp_err_t config_storage_save(const led_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, NVS_KEY_STATE, cfg, sizeof(led_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_storage_load(led_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t size = sizeof(led_config_t);
    err = nvs_get_blob(h, NVS_KEY_STATE, cfg, &size);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Load failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Version check — fall back to defaults if schema changed */
    if (cfg->version != CONFIG_STORAGE_VERSION) {
        ESP_LOGW(TAG, "Config version mismatch (got %u, want %u), using defaults",
                 cfg->version, CONFIG_STORAGE_VERSION);
        config_storage_defaults(cfg);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Config loaded: rgb=%s lvl=%u mode=%u hue=%u sat=%u w=%s wlvl=%u",
             cfg->rgb_on ? "on" : "off", cfg->rgb_level, cfg->color_mode,
             cfg->hue, cfg->saturation,
             cfg->white_on ? "on" : "off", cfg->white_level);
    return ESP_OK;
}

esp_err_t config_storage_save_led_count(uint16_t count)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, NVS_KEY_LED_COUNT, count);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save led_count failed: %s", esp_err_to_name(err));
    }
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

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

void config_storage_defaults(led_config_t *cfg)
{
    cfg->version     = CONFIG_STORAGE_VERSION;
    cfg->rgb_on      = false;
    cfg->rgb_level   = 128;
    cfg->color_x     = 0x616B;
    cfg->color_y     = 0x607D;
    cfg->hue         = 0;
    cfg->saturation  = 0;
    cfg->color_mode  = 0;   /* HS */
    cfg->white_on    = false;
    cfg->white_level = 128;
}
