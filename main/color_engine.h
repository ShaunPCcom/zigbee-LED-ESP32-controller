/**
 * @file color_engine.h
 * @brief Color conversion and hue utilities
 *
 * Provides comprehensive color space conversion and hue manipulation:
 * - HSV to RGB conversion with wraparound handling
 * - CIE 1931 XY chromaticity conversion (RGB ↔ XY)
 * - Hue normalization and shortest arc calculation
 * - Transition utilities for smooth color changes
 */

#ifndef COLOR_ENGINE_H
#define COLOR_ENGINE_H

#include <stdint.h>
#include "transition_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  HSV to RGB Conversion                                             */
/* ================================================================== */

/**
 * @brief Convert HSV color to RGB with wraparound-safe hue handling
 *
 * Converts HSV (Hue, Saturation, Value) to RGB (Red, Green, Blue).
 * Handles hue wraparound: negative values (represented as large uint16_t)
 * are normalized to 0-360 range before conversion.
 *
 * @param h  Hue (0-360 degrees, or wrapped negative as uint16_t)
 * @param s  Saturation (0-254, Zigbee scale)
 * @param v  Value/Brightness (0-255)
 * @param r  Output: Red component (0-255)
 * @param g  Output: Green component (0-255)
 * @param b  Output: Blue component (0-255)
 */
void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b);

/* ================================================================== */
/*  CIE 1931 XY Chromaticity Conversion                               */
/* ================================================================== */

/**
 * @brief Convert RGB to CIE 1931 XY chromaticity
 *
 * Converts 8-bit RGB values to CIE XY coordinates using standard
 * sRGB → XYZ → xy conversion with gamma correction.
 *
 * @param r  Red component (0-255)
 * @param g  Green component (0-255)
 * @param b  Blue component (0-255)
 * @param x  Output: CIE x coordinate (0-65535, representing 0.0-1.0)
 * @param y  Output: CIE y coordinate (0-65535, representing 0.0-1.0)
 */
void rgb_to_xy(uint8_t r, uint8_t g, uint8_t b, uint16_t *x, uint16_t *y);

/**
 * @brief Convert CIE 1931 XY chromaticity to RGB
 *
 * Converts CIE XY coordinates plus brightness to 8-bit RGB values
 * using standard xy → XYZ → sRGB conversion with gamma correction.
 *
 * @param x      CIE x coordinate (0-65535, representing 0.0-1.0)
 * @param y      CIE y coordinate (0-65535, representing 0.0-1.0)
 * @param level  Brightness level (0-255)
 * @param r      Output: Red component (0-255)
 * @param g      Output: Green component (0-255)
 * @param b      Output: Blue component (0-255)
 */
void xy_to_rgb(uint16_t x, uint16_t y, uint8_t level, uint8_t *r, uint8_t *g, uint8_t *b);

/* ================================================================== */
/*  Hue Manipulation Utilities                                        */
/* ================================================================== */

/**
 * @brief Convert ZCL hue (0-254) to degrees (0-360)
 *
 * Zigbee uses 0-254 range for hue; this converts to standard 0-360 degrees.
 *
 * @param zcl_hue  Zigbee ZCL hue value (0-254)
 * @return Hue in degrees (0-360)
 */
uint16_t zcl_hue_to_degrees(uint8_t zcl_hue);

/**
 * @brief Normalize hue value to 0-360 range, handling wraparound
 *
 * Converts wrapped negative values (e.g., -60 as 65476) back to 0-360.
 *
 * @param hue_raw  Raw hue from transition engine (may be wrapped)
 * @return Normalized hue in 0-360 range
 */
uint16_t normalize_hue(uint16_t hue_raw);

/**
 * @brief Calculate shortest hue arc for wraparound transitions
 *
 * Adjusts target_hue so the transition takes the shortest path around
 * the color wheel. Result may be negative or > 360 (will wrap as uint16_t).
 *
 * @param current_hue  Current hue value (0-360, normalized)
 * @param target_hue   Desired target hue (0-360, normalized)
 * @return Adjusted target for shortest arc (may be negative or > 360)
 */
int16_t hue_shortest_arc(uint16_t current_hue, uint16_t target_hue);

/**
 * @brief Start a hue transition with automatic shortest-arc calculation
 *
 * Always calculates shortest path around color wheel, even if current
 * transition value is wrapped from a previous arc-adjusted transition.
 *
 * @param hue_trans    Pointer to hue transition_t
 * @param target_hue   Desired target hue (0-360)
 * @param duration_ms  Transition duration in milliseconds
 */
void start_hue_transition(transition_t *hue_trans, uint16_t target_hue, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif // COLOR_ENGINE_H
