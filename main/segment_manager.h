/**
 * @file segment_manager.h
 * @brief Virtual segment state and rendering API
 *
 * 8 virtual segments on a single LED strip, each an Extended Color Light.
 * Color mode HS/XY drives RGB; CT mode drives the White channel.
 * Segment 1 defaults to the full strip length as the base layer.
 */

#ifndef SEGMENT_MANAGER_H
#define SEGMENT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "board_config.h"
#include "transition_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Geometry of a single segment (persisted as "seg_geom" blob)
 */
typedef struct {
    uint16_t start;    /* First LED index */
    uint16_t count;    /* Number of LEDs (0 = segment disabled) */
    uint8_t  strip_id; /* Physical strip index (0 or 1) */
} segment_geom_t;

/**
 * @brief Light state of a single segment (persisted as "seg_state" blob)
 *
 * color_mode: 0=Enhanced Hue, 2=CT
 *   Enhanced Hue -> RGB channels active (16-bit hue for full 360° precision), W=0
 *   CT -> W channel active (level = brightness), RGB=0
 *
 * startup_on_off: ZCL StartUpOnOff (attr 0x4003)
 *   0x00 = off, 0x01 = on, 0x02 = toggle, 0xFF = previous
 */
typedef struct {
    bool     on;
    uint8_t  level;          /* Brightness 0-254 */
    uint16_t hue;            /* Enhanced hue 0-360 degrees (stored as 0-360) */
    uint8_t  saturation;     /* Saturation 0-254 */
    uint8_t  color_mode;     /* 0=Enhanced Hue, 2=CT */
    uint16_t color_temp;     /* Color temperature in mireds (CT mode) */
    uint8_t  startup_on_off; /* Power-on behavior (ZCL StartUpOnOff) */
    /* Transition engine instances (not persisted — runtime only) */
    transition_t level_trans; /* brightness 0-254 */
    transition_t hue_trans;   /* enhanced hue 0-360 degrees */
    transition_t sat_trans;   /* saturation 0-254 */
    transition_t ct_trans;    /* color temp in mireds */
} segment_light_t;

/**
 * @brief Initialise segment manager with defaults
 *
 * @param default_count  LED count to assign to segment 1 (full strip default).
 *                       Pass g_led_count so segment 1 covers the whole strip.
 */
void segment_manager_init(uint16_t default_count);

/**
 * @brief Initialise transition current_values from the NVS-loaded state.
 *
 * Call after segment_manager_load() and before registering transitions with
 * the transition engine.  Sets each transition's current_value to match the
 * persisted state so the engine starts from the correct value (not 0).
 */
void segment_manager_init_transitions(void);

/**
 * @brief Get pointer to geometry array (MAX_SEGMENTS entries)
 */
segment_geom_t *segment_geom_get(void);

/**
 * @brief Get pointer to light state array (MAX_SEGMENTS entries)
 */
segment_light_t *segment_state_get(void);

/**
 * @brief Load segment state from NVS (call after config_storage_init)
 */
void segment_manager_load(void);

/**
 * @brief Save segment state to NVS
 */
void segment_manager_save(void);

#ifdef __cplusplus
}
#endif

#endif // SEGMENT_MANAGER_H
