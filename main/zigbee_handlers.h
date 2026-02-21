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
 * @brief C wrappers for C++ BoardLed API (implemented in main.cpp)
 */
void board_led_set_state_off(void);
void board_led_set_state_not_joined(void);
void board_led_set_state_pairing(void);
void board_led_set_state_joined(void);
void board_led_set_state_error(void);

/**
 * @brief Network joined state (for button LED feedback)
 */
extern bool s_network_joined;

/**
 * @brief Sync ZCL attribute store from segment state
 *
 * Updates Zigbee attributes to match current segment state (used after preset recall)
 */
void sync_zcl_from_state(void);

/**
 * @brief Update LED strip output
 *
 * Renders current segment states to the physical LED strips
 */
void update_leds(void);

/**
 * @brief Schedule NVS save (debounced)
 *
 * Queues a save operation with debounce to avoid excessive NVS writes
 */
void schedule_save(void);

/**
 * @brief Schedule deferred ZCL sync from Zigbee task context
 *
 * Uses esp_zb_scheduler_alarm to defer sync_zcl_from_state() execution
 * to Zigbee task, avoiding critical section mismatch when called from
 * non-Zigbee tasks (e.g., CLI)
 */
void schedule_zcl_sync(void);

/**
 * @brief Get the current global transition duration
 *
 * @return Duration in milliseconds (0 = instant)
 */
uint16_t zigbee_handlers_get_global_transition_ms(void);

/**
 * @brief Set the global transition duration
 *
 * Updates the in-memory value and saves to NVS.
 *
 * @param ms  Duration in milliseconds (0 = instant, max 65535)
 */
void zigbee_handlers_set_global_transition_ms(uint16_t ms);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_HANDLERS_H
