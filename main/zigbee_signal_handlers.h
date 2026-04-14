/**
 * @file zigbee_signal_handlers.h
 * @brief LED-specific Zigbee lifecycle hooks and deferred ZCL sync.
 *
 * The common network lifecycle handler (esp_zb_app_signal_handler,
 * zigbee_factory_reset, zigbee_full_factory_reset, reboot_cb, etc.) is
 * provided by the shared zigbee_signal_handler component. This header
 * re-exports those declarations and adds LED-specific additions.
 */

#ifndef ZIGBEE_SIGNAL_HANDLERS_H
#define ZIGBEE_SIGNAL_HANDLERS_H

#include "zigbee_signal_handler.h"  /* re-exports common API */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register LED-specific Zigbee lifecycle hooks.
 *
 * Must be called before the Zigbee stack starts (i.e. before zigbee_init()).
 * Registers on_joined, on_left, on_stack_init callbacks and the NVS namespace
 * for full factory reset.
 */
void zigbee_signal_handlers_setup(void);

/**
 * @brief Schedule a deferred sync of ZCL attributes from current LED state.
 *
 * Uses esp_zb_scheduler_alarm to defer sync_zcl_from_state() into the Zigbee
 * task, avoiding FreeRTOS critical section violations when called from CLI
 * or other non-Zigbee task contexts.
 */
void schedule_zcl_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_SIGNAL_HANDLERS_H */
