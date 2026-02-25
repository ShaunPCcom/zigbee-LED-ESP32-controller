/**
 * @file led_renderer.h
 * @brief LED render loop, ZCL polling, and state synchronization
 *
 * Handles the 200Hz render/poll loop for segment state and physical LED updates.
 * Polls Zigbee ZCL attributes for HS/CT color changes (SDK handles internally).
 * Manages ZCL attribute store synchronization after state changes.
 */

#ifndef LED_RENDERER_H
#define LED_RENDERER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Global transition duration (milliseconds)
 *
 * Default transition duration for preset recalls and other operations.
 * Default: 0ms (instant). Modified via setter function.
 */
extern uint16_t g_global_transition_ms;

/**
 * @brief Get global transition duration for preset recalls and explicit commands
 * @return Transition duration in milliseconds (0 = instant)
 */
uint16_t led_renderer_get_global_transition_ms(void);

/**
 * @brief Set global transition duration for preset recalls and explicit commands
 * @param ms Transition duration in milliseconds (0 = instant)
 */
void led_renderer_set_global_transition_ms(uint16_t ms);

/**
 * @brief Schedule segment configuration save to NVS (500ms debounce)
 *
 * Defers save operation to avoid excessive NVS writes during rapid changes.
 * Timer is restarted on each call, so only the final state is saved.
 */
void schedule_save(void);

/**
 * @brief Schedule ZCL attribute store sync from in-memory state (100ms deferred)
 *
 * Used after preset recall to avoid Zigbee stack assertion from immediate sync.
 * Defers sync operation to Zigbee task context via scheduler alarm.
 */
void schedule_zcl_sync(void);

/**
 * @brief Synchronize ZCL attribute store from current segment state
 *
 * Pushes in-memory segment state to ZCL attribute store for all endpoints.
 * Used after boot (to restore NVS state) and after preset recall.
 * CAUTION: Cannot be called from attribute handler context (causes assertion).
 */
void sync_zcl_from_state(void);

/**
 * @brief Update physical LEDs from current segment state and transition values
 *
 * Renders all segments to LED strip buffers (segment 1 = base layer, 8 = top).
 * Reads interpolated values from transition engine for smooth animations.
 * Called at 200Hz by render loop.
 */
void update_leds(void);

/**
 * @brief Deferred LED update callback for scheduler alarm
 * @param param Unused (required by scheduler alarm signature)
 *
 * Used after network join or other deferred LED update triggers.
 */
void restore_leds_cb(uint8_t param);

/**
 * @brief Start 200Hz LED render/poll loop
 *
 * Starts continuous 5ms scheduler alarm for segment polling and LED rendering.
 * Polls ZCL attributes for HS/CT changes (SDK handles commands internally).
 * Must be called after Zigbee network join.
 */
void led_renderer_start(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_RENDERER_H */
