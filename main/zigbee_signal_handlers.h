/**
 * @file zigbee_signal_handlers.h
 * @brief Zigbee signal handler and factory reset
 */

#ifndef ZIGBEE_SIGNAL_HANDLERS_H
#define ZIGBEE_SIGNAL_HANDLERS_H

#include "esp_zigbee_core.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief Schedule deferred ZCL sync from Zigbee task context
 *
 * Uses esp_zb_scheduler_alarm to defer sync_zcl_from_state() execution
 * to Zigbee task, avoiding critical section mismatch when called from
 * non-Zigbee tasks (e.g., CLI)
 */
void schedule_zcl_sync(void);

/**
 * @brief Reboot callback for deferred restart
 *
 * Exposed for use by attribute handlers that need deferred reboot
 */
void reboot_cb(uint8_t param);

/**
 * @brief Network joined state (for button LED feedback)
 */
extern bool s_network_joined;

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_SIGNAL_HANDLERS_H
