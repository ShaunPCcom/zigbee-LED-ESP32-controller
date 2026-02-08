/**
 * @file config_storage.h
 * @brief NVS-backed persistence for LED state
 */

#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_STORAGE_VERSION  1

/**
 * @brief Persisted LED state
 */
typedef struct {
    uint8_t  version;
    bool     rgb_on;
    uint8_t  rgb_level;
    uint16_t color_x;
    uint16_t color_y;
    uint16_t hue;
    uint8_t  saturation;
    uint8_t  color_mode;    /* 0=HS, 1=XY */
    bool     white_on;
    uint8_t  white_level;
} led_config_t;

/**
 * @brief Initialise storage (call after nvs_flash_init)
 */
esp_err_t config_storage_init(void);

/**
 * @brief Save LED config to NVS
 */
esp_err_t config_storage_save(const led_config_t *cfg);

/**
 * @brief Load LED config from NVS
 * @return ESP_OK with populated cfg, or ESP_ERR_NOT_FOUND if no saved state
 */
esp_err_t config_storage_load(led_config_t *cfg);

/**
 * @brief Populate cfg with factory defaults
 */
void config_storage_defaults(led_config_t *cfg);

/**
 * @brief Save LED strip count to NVS
 */
esp_err_t config_storage_save_led_count(uint16_t count);

/**
 * @brief Load LED strip count from NVS
 * @return ESP_OK with populated count, or ESP_ERR_NOT_FOUND if not set
 */
esp_err_t config_storage_load_led_count(uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_STORAGE_H
