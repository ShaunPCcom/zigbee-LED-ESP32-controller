/**
 * @file preset_handler.h
 * @brief Preset ZCL integration - bridge between Zigbee and preset_manager
 *
 * Handles Zigbee attribute writes for preset recall/save/delete operations.
 * Bridges ZCL cluster 0xFC02 to the preset_manager C++ module.
 *
 * Supports both slot-based (current) and deprecated name-based preset APIs.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Update preset ZCL attributes from preset_manager state
 *
 * Refreshes preset_count, active_preset (deprecated), and preset_N_name 
 * attributes (slots 0-7) by reading current state from preset_manager.
 * Call after any preset save/delete/recall operation.
 */
void update_preset_zcl_attrs(void);

/**
 * @brief Handle recall_slot attribute write (0x0020)
 *
 * Recalls preset from specified slot, starts transitions, and syncs ZCL.
 * Clears recall_slot attribute to 0xFF after operation.
 *
 * @param slot Slot number (0-7)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range,
 *         ESP_ERR_NOT_FOUND if slot empty
 */
esp_err_t handle_recall_slot_write(uint8_t slot);

/**
 * @brief Handle save_slot attribute write (0x0021)
 *
 * Saves current segment state to specified slot with optional name.
 * Uses pending save name (from handle_save_name_write) if set, otherwise default.
 * Clears save_slot attribute to 0xFF after operation.
 *
 * @param slot Slot number (0-7)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range
 */
esp_err_t handle_save_slot_write(uint8_t slot);

/**
 * @brief Handle delete_slot attribute write (0x0022)
 *
 * Deletes preset from specified slot and syncs ZCL.
 * Clears delete_slot attribute to 0xFF after operation.
 *
 * @param slot Slot number (0-7)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if slot out of range,
 *         ESP_ERR_NOT_FOUND if slot already empty
 */
esp_err_t handle_delete_slot_write(uint8_t slot);

/**
 * @brief Handle save_name attribute write (0x0023)
 *
 * Stores name for next save_slot operation. Name is cleared after save.
 * ZCL CharString format: first byte is length, rest is name.
 *
 * @param char_str ZCL CharString value (length-prefixed)
 * @return ESP_OK always
 */
esp_err_t handle_save_name_write(const uint8_t *char_str);

/**
 * @brief Handle deprecated name-based preset operations (backwards compatibility)
 *
 * Handles recall_preset (0x0002), save_preset (0x0003), delete_preset (0x0004)
 * attributes using name-based preset_manager APIs.
 *
 * @param attr_id ZCL attribute ID (ZB_ATTR_RECALL_PRESET, etc.)
 * @param value Pointer to ZCL CharString value (length-prefixed)
 * @return ESP_OK always
 */
esp_err_t handle_deprecated_preset_write(uint16_t attr_id, const void *value);

#ifdef __cplusplus
}
#endif
