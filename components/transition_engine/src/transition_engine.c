/**
 * @file transition_engine.c
 * @brief Generic transition engine implementation.
 *
 * The engine maintains a static registry of transition_t pointers.
 * A periodic esp_timer fires at the configured update rate and calls
 * transition_tick() on every registered, active transition.
 *
 * Memory ownership: callers embed transition_t in their own structs.
 * The registry stores only pointers — no memory is allocated here.
 */

#include "transition_engine.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TRANSITION_REGISTRY_MAX 64

static const char *TAG = "transition_engine";

/* Registry of caller-owned transition_t pointers */
static transition_t *s_registry[TRANSITION_REGISTRY_MAX];
static int           s_registry_count = 0;

static esp_timer_handle_t s_timer = NULL;

/* ------------------------------------------------------------------ */
/* Timer callback                                                       */
/* ------------------------------------------------------------------ */

static void timer_callback(void *arg)
{
    for (int i = 0; i < s_registry_count; i++) {
        transition_t *t = s_registry[i];
        if (t != NULL && t->active) {
            transition_tick(t);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t transition_engine_init(uint16_t update_rate_hz)
{
    if (s_timer != NULL) {
        ESP_LOGD(TAG, "already initialized");
        return ESP_OK;
    }

    if (update_rate_hz == 0) {
        update_rate_hz = 200;
    }

    uint64_t period_us = 1000000ULL / update_rate_hz;

    const esp_timer_create_args_t timer_args = {
        .callback        = timer_callback,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "transition_engine",
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_timer_create failed: %d", err);
        return err;
    }

    err = esp_timer_start_periodic(s_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_timer_start_periodic failed: %d", err);
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return err;
    }

    ESP_LOGD(TAG, "initialized at %u Hz (period %llu us)", update_rate_hz, period_us);
    return ESP_OK;
}

esp_err_t transition_register(transition_t *t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Idempotent: check if already registered */
    for (int i = 0; i < s_registry_count; i++) {
        if (s_registry[i] == t) {
            ESP_LOGD(TAG, "transition %p already registered", (void *)t);
            return ESP_OK;
        }
    }

    if (s_registry_count >= TRANSITION_REGISTRY_MAX) {
        ESP_LOGD(TAG, "registry full (%d entries)", TRANSITION_REGISTRY_MAX);
        return ESP_ERR_NO_MEM;
    }

    s_registry[s_registry_count++] = t;
    ESP_LOGD(TAG, "registered transition %p (total: %d)", (void *)t, s_registry_count);
    return ESP_OK;
}

void transition_start(transition_t *t, uint16_t target, uint32_t duration_ms)
{
    if (t == NULL) {
        return;
    }

    /* Instant transition: skip interpolation entirely */
    if (duration_ms == 0) {
        t->start_value   = target;
        t->target_value  = target;
        t->current_value = target;
        t->active        = false;
        ESP_LOGD(TAG, "instant transition %p -> %u", (void *)t, target);
        return;
    }

    /*
     * Capture the current interpolated position as the new start value.
     * This handles both:
     *   - Fresh start:      current_value is 0 (zero-initialised struct), so
     *                       the transition begins from whatever the caller
     *                       has set (or 0 by default).
     *   - Interruption:     current_value holds the mid-flight value, so
     *                       the new transition continues from that exact point
     *                       with no visual jump.
     */
    t->start_value  = t->current_value;
    t->target_value = target;
    t->duration_us  = (uint32_t)((uint64_t)duration_ms * 1000ULL);
    t->start_time_us = esp_timer_get_time();
    t->active        = true;

    ESP_LOGD(TAG, "start transition %p: %u -> %u over %u ms",
             (void *)t, t->start_value, target, duration_ms);
}

uint16_t transition_get_value(const transition_t *t)
{
    if (t == NULL) {
        return 0;
    }
    return t->current_value;
}

bool transition_is_active(const transition_t *t)
{
    if (t == NULL) {
        return false;
    }
    return t->active;
}

void transition_cancel(transition_t *t)
{
    if (t == NULL) {
        return;
    }
    /* Freeze at current interpolated position, do not snap to target */
    t->active = false;
    ESP_LOGD(TAG, "cancelled transition %p, frozen at %u", (void *)t, t->current_value);
}

void transition_tick(transition_t *t)
{
    if (t == NULL || !t->active) {
        return;
    }

    int64_t now     = esp_timer_get_time();
    int64_t elapsed = now - t->start_time_us;

    if (elapsed < 0) {
        /* Clock skew guard: treat as start of transition */
        elapsed = 0;
    }

    if ((uint64_t)elapsed >= (uint64_t)t->duration_us) {
        /* Transition complete */
        t->current_value = t->target_value;
        t->active        = false;
        ESP_LOGD(TAG, "transition %p complete -> %u", (void *)t, t->target_value);
        return;
    }

    /* Linear interpolation:
     *   value = start + (range * elapsed) / duration
     *
     * Uses int64 for the multiply to avoid overflow when range and
     * duration_us are both large (range up to 65535, duration up to ~4B us).
     */
    int32_t range = (int32_t)t->target_value - (int32_t)t->start_value;
    int32_t val   = (int32_t)t->start_value +
                    (int32_t)(((int64_t)range * elapsed) / (int64_t)t->duration_us);

    t->current_value = (uint16_t)val;
}
