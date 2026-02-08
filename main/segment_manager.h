/**
 * @file segment_manager.h
 * @brief Virtual segment state and rendering API
 *
 * Supports up to MAX_SEGMENTS virtual segments overlaid on a single LED strip.
 * Each segment has configurable start index, LED count, and independent
 * color/brightness/on state.
 */

#ifndef SEGMENT_MANAGER_H
#define SEGMENT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Geometry of a single segment (persisted as "seg_geom" blob)
 */
typedef struct {
    uint16_t start;       /* First LED index */
    uint16_t count;       /* Number of LEDs (0 = segment disabled) */
    uint8_t  white_level; /* Independent white channel level (0-254) */
} segment_geom_t;

/**
 * @brief Light state of a single segment (persisted as "seg_state" blob)
 */
typedef struct {
    bool     on;
    uint8_t  level;      /* Brightness 0-254 */
    uint16_t hue;        /* 0-360 degrees */
    uint8_t  saturation; /* 0-254 */
    uint8_t  color_mode; /* 0=HS, 1=XY */
    uint16_t color_x;    /* CIE X (ZCL format) */
    uint16_t color_y;    /* CIE Y (ZCL format) */
} segment_light_t;

/**
 * @brief Initialise segment manager with defaults
 */
void segment_manager_init(void);

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
 * @brief Save segment state to NVS (debounced externally)
 */
void segment_manager_save(void);

#ifdef __cplusplus
}
#endif

#endif // SEGMENT_MANAGER_H
