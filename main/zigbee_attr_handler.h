/**
 * @file zigbee_attr_handler.h
 * @brief Zigbee attribute write dispatch and action handler
 *
 * Handles incoming Zigbee ZCL attribute writes and dispatches to appropriate
 * handlers (segment control, device config, preset operations).
 */

#ifndef ZIGBEE_ATTR_HANDLER_H
#define ZIGBEE_ATTR_HANDLER_H

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zigbee core action handler
 *
 * Top-level dispatcher for Zigbee action callbacks. Routes OTA callbacks
 * to OTA component and attribute writes to handle_set_attr_value().
 *
 * @param callback_id  Type of callback (SET_ATTR_VALUE, etc.)
 * @param message      Callback-specific message structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_ATTR_HANDLER_H
