/**
 * @file preset_manager.h
 * @brief Manage slot-based presets for segment states
 *
 * Saves/recalls all 8 segment states as slot-based presets. 8 preset slots (0-7).
 * Each preset stores name + 8 segment_light_t structs in NVS.
 */

#ifndef PRESET_MANAGER_H
#define PRESET_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PRESET_SLOTS 8
#define PRESET_NAME_MAX  16

/**
 * @brief Initialize preset manager, perform migration if needed
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t preset_manager_init(void);

/**
 * @brief Save current segment states to a preset slot
 * @param slot Slot index (0-7)
 * @param name Preset name (max 16 chars, UTF-8), or NULL for "Preset N"
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range, error code otherwise
 */
esp_err_t preset_manager_save(uint8_t slot, const char *name);

/**
 * @brief Recall a preset from a slot
 * @param slot Slot index (0-7)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range,
 *         ESP_ERR_NOT_FOUND if slot empty, error code otherwise
 */
esp_err_t preset_manager_recall(uint8_t slot);

/**
 * @brief Delete a preset from a slot
 * @param slot Slot index (0-7)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range, error code otherwise
 */
esp_err_t preset_manager_delete(uint8_t slot);

/**
 * @brief Get the name of a preset slot
 * @param slot Slot index (0-7)
 * @param name_out Buffer to write name into (must be at least PRESET_NAME_MAX+1 bytes)
 * @param max_len Buffer size
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range or slot empty
 */
esp_err_t preset_manager_get_slot_name(uint8_t slot, char *name_out, size_t max_len);

/**
 * @brief Check if a preset slot is occupied
 * @param slot Slot index (0-7)
 * @param is_occupied Output: true if slot has data, false if empty
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range or is_occupied is NULL
 */
esp_err_t preset_manager_is_slot_occupied(uint8_t slot, bool *is_occupied);

/**
 * @brief List all preset slots with names and status
 */
void preset_manager_list_presets(void);

/* ================================================================== */
/*  Compatibility functions for Zigbee handlers (deprecated)          */
/*  Kept for backwards compatibility with old Z2M converters          */
/* ================================================================== */

/**
 * @brief Get count of occupied slots (compatibility)
 * @return Number of occupied slots (0-8)
 */
int preset_manager_count(void);

/**
 * @brief Get active preset name (compatibility stub)
 * @return Empty string (active tracking removed in v2)
 */
const char *preset_manager_get_active(void);

/**
 * @brief Recall preset by name (compatibility bridge)
 * @param name Preset name
 * @return true on success, false on error
 */
bool preset_manager_recall_by_name(const char *name);

/**
 * @brief Save preset by name (compatibility bridge)
 * @param name Preset name
 * @return true on success, false on error
 */
bool preset_manager_save_by_name(const char *name);

/**
 * @brief Delete preset by name (compatibility bridge)
 * @param name Preset name
 * @return true on success, false on error
 */
bool preset_manager_delete_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif // PRESET_MANAGER_H
