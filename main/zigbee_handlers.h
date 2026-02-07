/**
 * @file zigbee_handlers.h
 * @brief Zigbee command and signal handlers
 *
 * Handles incoming Zigbee commands (On/Off, Level, Color) and
 * updates the LED strip accordingly.
 */

#ifndef ZIGBEE_HANDLERS_H
#define ZIGBEE_HANDLERS_H

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zigbee core action handler
 *
 * Processes Zigbee attribute changes and commands
 */
esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

/**
 * @brief Zigbee signal handler
 *
 * Handles Zigbee stack signals (join, leave, etc.)
 * This must be named esp_zb_app_signal_handler for the Zigbee stack
 */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct);

/**
 * @brief Zigbee network reset (keeps config)
 *
 * Leaves the Zigbee network but preserves device configuration
 */
void zigbee_factory_reset(void);

/**
 * @brief Full factory reset (Zigbee + NVS config)
 *
 * Erases both Zigbee network data and NVS configuration
 */
void zigbee_full_factory_reset(void);

/**
 * @brief Start button monitoring task
 *
 * Monitors boot button for factory reset functions
 */
void button_task_start(void);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_HANDLERS_H
