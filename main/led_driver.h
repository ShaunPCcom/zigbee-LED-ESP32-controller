/**
 * @file led_driver.h
 * @brief LED strip driver using SPI2 with time-multiplexed dual strip support
 *
 * Two SK6812 RGBW strips share a single SPI2 bus. Before each transmission
 * the MOSI GPIO is switched via the GPIO matrix (GPIO4 for strip 0, GPIO5
 * for strip 1). Strip with count=0 is skipped entirely.
 *
 * Timing: 2.5 MHz SPI, 3 SPI bits per LED bit:
 *   0 -> 100  (high 400 ns, low 800 ns)
 *   1 -> 110  (high 800 ns, low 400 ns)
 */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_DRIVER_MAX_STRIPS  2

/**
 * @brief LED strip type
 */
typedef enum {
    LED_STRIP_TYPE_SK6812  = 0,  /**< SK6812 RGBW (4 bytes/LED, GRBW order) */
    LED_STRIP_TYPE_WS2812B = 1,  /**< WS2812B RGB  (3 bytes/LED, GRB order)  */
} led_strip_type_t;

/**
 * @brief Initialize LED driver and SPI bus
 *
 * @param count0  LED count for strip 0 (GPIO4), 0 = disabled
 * @param count1  LED count for strip 1 (GPIO5), 0 = disabled
 * @param type0   Strip 0 LED type (SK6812 or WS2812B)
 * @param type1   Strip 1 LED type
 */
esp_err_t led_driver_init(uint16_t count0, uint16_t count1,
                           led_strip_type_t type0, led_strip_type_t type1);

/**
 * @brief Set an RGBW pixel in the buffer for a specific strip
 */
esp_err_t led_driver_set_pixel(uint8_t strip, uint16_t idx,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t w);

/**
 * @brief Clear (zero) the pixel buffer for a specific strip (no transmit)
 */
esp_err_t led_driver_clear(uint8_t strip);

/**
 * @brief Transmit both strip buffers via SPI (time-multiplexed)
 */
esp_err_t led_driver_refresh(void);

/**
 * @brief Get the LED count for a specific strip
 */
uint16_t led_driver_get_count(uint8_t strip);

/**
 * @brief Get the LED type for a specific strip
 */
led_strip_type_t led_driver_get_type(uint8_t strip);

#ifdef __cplusplus
}
#endif

#endif // LED_DRIVER_H
