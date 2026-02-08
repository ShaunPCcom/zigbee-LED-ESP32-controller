/**
 * @file config_storage.h
 * @brief NVS persistence for device configuration
 *
 * Stores per-strip LED counts. Segment state is managed separately
 * by segment_manager.c using the same "led_cfg" NVS namespace.
 */

#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise storage (call after nvs_flash_init)
 */
esp_err_t config_storage_init(void);

/**
 * @brief Save LED strip count for a specific strip (0 or 1) to NVS
 */
esp_err_t config_storage_save_strip_count(uint8_t strip, uint16_t count);

/**
 * @brief Load LED strip count for a specific strip (0 or 1) from NVS
 * @return ESP_OK with populated count, or ESP_ERR_NOT_FOUND if not set
 */
esp_err_t config_storage_load_strip_count(uint8_t strip, uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_STORAGE_H
