/**
 * @file color_conversion.h
 * @brief CIE 1931 XY color space conversion utilities
 *
 * Provides conversion between RGB and CIE 1931 XY chromaticity coordinates
 * for accurate color representation in Zigbee color control.
 */

#ifndef COLOR_CONVERSION_H
#define COLOR_CONVERSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif // COLOR_CONVERSION_H
