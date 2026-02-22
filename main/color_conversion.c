/**
 * @file color_conversion.c
 * @brief CIE 1931 XY color space conversion implementation
 *
 * Based on standard sRGB ↔ XYZ ↔ xy conversion formulas.
 * Uses D65 illuminant (standard daylight) as reference white point.
 */

#include "color_conversion.h"
#include <math.h>

/* sRGB gamma correction (linearization) */
static float gamma_correct(uint8_t value)
{
    float v = value / 255.0f;
    if (v <= 0.04045f) {
        return v / 12.92f;
    } else {
        return powf((v + 0.055f) / 1.055f, 2.4f);
    }
}

/* Inverse sRGB gamma (for XYZ → RGB) */
static uint8_t gamma_inverse(float value)
{
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;

    float v;
    if (value <= 0.0031308f) {
        v = value * 12.92f;
    } else {
        v = 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
    }

    return (uint8_t)(v * 255.0f + 0.5f);
}

void rgb_to_xy(uint8_t r, uint8_t g, uint8_t b, uint16_t *x, uint16_t *y)
{
    /* Gamma correct RGB → linear RGB */
    float R = gamma_correct(r);
    float G = gamma_correct(g);
    float B = gamma_correct(b);

    /* sRGB → XYZ conversion matrix (D65 illuminant) */
    float X = R * 0.4124564f + G * 0.3575761f + B * 0.1804375f;
    float Y = R * 0.2126729f + G * 0.7151522f + B * 0.0721750f;
    float Z = R * 0.0193339f + G * 0.1191920f + B * 0.9503041f;

    /* XYZ → xy chromaticity */
    float sum = X + Y + Z;
    if (sum < 0.00001f) {
        /* Black or very dark - use D65 white point */
        *x = (uint16_t)(0.31271f * 65535.0f);
        *y = (uint16_t)(0.32902f * 65535.0f);
    } else {
        float x_val = X / sum;
        float y_val = Y / sum;

        /* Clamp to valid range [0.0, 1.0] */
        if (x_val < 0.0f) x_val = 0.0f;
        if (x_val > 1.0f) x_val = 1.0f;
        if (y_val < 0.0f) y_val = 0.0f;
        if (y_val > 1.0f) y_val = 1.0f;

        /* Convert to 16-bit fixed point */
        *x = (uint16_t)(x_val * 65535.0f + 0.5f);
        *y = (uint16_t)(y_val * 65535.0f + 0.5f);
    }
}

void xy_to_rgb(uint16_t x, uint16_t y, uint8_t level, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Convert 16-bit fixed point to float [0.0, 1.0] */
    float x_val = x / 65535.0f;
    float y_val = y / 65535.0f;

    /* Prevent division by zero */
    if (y_val < 0.00001f) {
        y_val = 0.00001f;
    }

    /* Calculate z from x + y + z = 1 */
    float z_val = 1.0f - x_val - y_val;

    /* Convert xy → XYZ (using brightness as Y) */
    float brightness = level / 255.0f;
    float Y = brightness;
    float X = (Y / y_val) * x_val;
    float Z = (Y / y_val) * z_val;

    /* XYZ → sRGB conversion matrix (D65 illuminant, inverse) */
    float R =  X *  3.2404542f + Y * -1.5371385f + Z * -0.4985314f;
    float G =  X * -0.9692660f + Y *  1.8760108f + Z *  0.0415560f;
    float B =  X *  0.0556434f + Y * -0.2040259f + Z *  1.0572252f;

    /* Clamp to valid range [0.0, 1.0] before gamma correction */
    if (R < 0.0f) R = 0.0f;
    if (R > 1.0f) R = 1.0f;
    if (G < 0.0f) G = 0.0f;
    if (G > 1.0f) G = 1.0f;
    if (B < 0.0f) B = 0.0f;
    if (B > 1.0f) B = 1.0f;

    /* Apply inverse gamma correction */
    *r = gamma_inverse(R);
    *g = gamma_inverse(G);
    *b = gamma_inverse(B);
}
