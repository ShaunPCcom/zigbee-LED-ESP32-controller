// SPDX-License-Identifier: MIT
/**
 * @file config_api.h
 * @brief Web API bridge to LED segment/preset/strip state (C6 only).
 *
 * All functions are safe to call from HTTP handler tasks.
 * State changes propagate immediately to the 200Hz render loop.
 */
#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the strip config JSON object.
 *        Caller must cJSON_Delete() the result.
 */
esp_err_t config_api_get_strip_config(cJSON **out);

/**
 * @brief Apply strip config from a JSON object (partial updates supported).
 *        Keys: strip1_count, strip2_count, transition_ms,
 *              strip1_type, strip2_type (0=SK6812 1=WS2812B),
 *              strip1_max_current, strip2_max_current (mA, 0=unlimited)
 */
esp_err_t config_api_set_strip_config(const cJSON *obj);

/**
 * @brief Build the segments JSON array.
 *        Caller must cJSON_Delete() the result.
 */
esp_err_t config_api_get_segments(cJSON **out);

/**
 * @brief Apply one or more segment updates from a JSON object.
 *        Accepts either { "index": N, ...fields } or { "segments": [...] }.
 */
esp_err_t config_api_set_segments(const cJSON *obj);

/**
 * @brief Build the presets JSON array (slot names and occupancy).
 *        Caller must cJSON_Delete() the result.
 */
esp_err_t config_api_get_presets(cJSON **out);

/**
 * @brief Apply (recall) a preset slot.
 * @param slot 0-7
 */
esp_err_t config_api_apply_preset(int slot);

/**
 * @brief Save current state to a preset slot.
 * @param slot 0-7
 * @param name  Preset name string (may be NULL or empty for auto-name)
 */
esp_err_t config_api_save_preset(int slot, const char *name);

/**
 * @brief Delete a preset slot.
 * @param slot 0-7
 */
esp_err_t config_api_delete_preset(int slot);

#ifdef __cplusplus
}
#endif
