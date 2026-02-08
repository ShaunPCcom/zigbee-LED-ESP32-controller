/**
 * @file preset_manager.h
 * @brief Manage named presets for segment states
 *
 * Saves/recalls all 8 segment states as named presets. Max 8 presets.
 * Each preset stores name + 8 segment_light_t structs in NVS.
 */

#ifndef PRESET_MANAGER_H
#define PRESET_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PRESETS      8
#define PRESET_NAME_MAX  16

/**
 * @brief Initialize preset manager and load presets from NVS
 */
void preset_manager_init(void);

/**
 * @brief Get number of stored presets
 * @return Count of non-empty preset slots (0-8)
 */
int preset_manager_count(void);

/**
 * @brief Save current segment states as a named preset
 * @param name Preset name (max 16 chars, null-terminated)
 * @return true if saved successfully, false on error
 */
bool preset_manager_save(const char *name);

/**
 * @brief Recall a preset by name
 * @param name Preset name to recall
 * @return true if recalled successfully, false if not found
 */
bool preset_manager_recall(const char *name);

/**
 * @brief Delete a preset by name
 * @param name Preset name to delete
 * @return true if deleted successfully, false if not found
 */
bool preset_manager_delete(const char *name);

/**
 * @brief Get the name of the last recalled preset
 * @return Pointer to active preset name (empty string if none)
 */
const char *preset_manager_get_active(void);

/**
 * @brief Get preset name from a specific slot
 * @param slot Slot index (0-7)
 * @param buf Buffer to write name into
 * @param len Buffer size
 * @return true if slot has a preset, false if empty
 */
bool preset_manager_get_name(int slot, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // PRESET_MANAGER_H
