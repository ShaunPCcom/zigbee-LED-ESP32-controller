/**
 * @file transition_engine.h
 * @brief Generic transition engine for smooth numeric value interpolation.
 *
 * Usage pattern:
 *   1. Embed a transition_t in your struct (segment, animation, etc.)
 *   2. Call transition_engine_init() once during app startup
 *   3. Call transition_register(&t) once per transition_t instance
 *   4. Call transition_start(&t, target, duration_ms) to begin
 *   5. Read current interpolated value with transition_get_value(&t)
 *   6. Apply value to hardware at your own update rate
 *
 * Interruption handling:
 *   Calling transition_start() on an already-active transition
 *   seamlessly begins a new transition FROM the current interpolated
 *   value to the new target. No visual jumps.
 *
 * Animation use:
 *   Animations embed their own transition_t fields and call
 *   transition_start() with custom durations. No limits on how many
 *   can exist simultaneously.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief State for a single transition. Embed one per transitioning value.
 *
 * Initialize to zero ({0}) before first use.
 * Do not access fields directly — use transition_* functions.
 */
typedef struct {
    bool     active;          /* True if transition in progress */
    int64_t  start_time_us;   /* esp_timer_get_time() at transition start */
    uint32_t duration_us;     /* Total duration in microseconds */
    uint16_t start_value;     /* Value at time transition was started */
    uint16_t target_value;    /* Destination value */
    uint16_t current_value;   /* Latest interpolated value (read-safe) */
} transition_t;

/**
 * @brief Initialize the transition engine timer.
 *
 * Must be called once at startup before any transitions are started.
 * Starts a periodic esp_timer that updates all registered transitions.
 *
 * @param update_rate_hz  Update rate in Hz (recommended: 200)
 * @return ESP_OK on success
 */
esp_err_t transition_engine_init(uint16_t update_rate_hz);

/**
 * @brief Register a transition_t for automatic updates.
 *
 * The engine maintains a lightweight list of registered transitions.
 * The timer callback calls transition_tick() on each registered entry.
 * Callers own the memory; the engine stores only a pointer.
 *
 * Only needs to be called once per transition_t instance.
 * Safe to call multiple times (idempotent).
 *
 * @param t  Pointer to caller-owned transition_t
 * @return ESP_OK on success, ESP_ERR_NO_MEM if registry full
 */
esp_err_t transition_register(transition_t *t);

/**
 * @brief Start or update a transition.
 *
 * If the transition is currently active, starts a new transition FROM
 * the current interpolated value to the new target — no visual jump.
 *
 * If duration_ms is 0, instantly sets value to target (no interpolation).
 *
 * @param t           Pointer to transition_t (caller owns)
 * @param target      Target value (0–65535)
 * @param duration_ms Duration in milliseconds (0 = instant)
 */
void transition_start(transition_t *t, uint16_t target, uint32_t duration_ms);

/**
 * @brief Get the current interpolated value.
 *
 * Safe to call from any context. Returns current_value (which equals
 * target_value when no transition is active).
 *
 * @param t  Pointer to transition_t
 * @return   Current interpolated value
 */
uint16_t transition_get_value(const transition_t *t);

/**
 * @brief Returns true if a transition is currently running.
 */
bool transition_is_active(const transition_t *t);

/**
 * @brief Cancel an active transition, snapping to the current value.
 *
 * Does not snap to target — freezes at current interpolated position.
 *
 * @param t  Pointer to transition_t
 */
void transition_cancel(transition_t *t);

/**
 * @brief Advance a single transition by one tick.
 *
 * Called automatically by the engine timer for registered transitions.
 * Can also be called manually if preferred (e.g. from an RTOS task).
 *
 * @param t  Pointer to transition_t
 */
void transition_tick(transition_t *t);

#ifdef __cplusplus
}
#endif
