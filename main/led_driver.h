/**
 * @file led_driver.h
 * @brief LED strip driver for WS2812B (RGB) and SK6812 (RGBW) using RMT peripheral
 *
 * This driver uses the ESP-IDF RMT (Remote Control) peripheral to generate
 * the precise timing signals required by WS2812B and SK6812 addressable LEDs.
 */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/rmt_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED strip type
 */
typedef enum {
    LED_STRIP_TYPE_RGB,     /*!< WS2812B - 3 bytes per LED (GRB order) */
    LED_STRIP_TYPE_RGBW,    /*!< SK6812 - 4 bytes per LED (GRBW order) */
} led_strip_type_t;

/**
 * @brief LED strip configuration
 */
typedef struct {
    gpio_num_t gpio_num;           /*!< GPIO number for LED data line */
    uint16_t led_count;            /*!< Number of LEDs in the strip */
    led_strip_type_t type;         /*!< LED strip type (RGB or RGBW) */
    uint32_t rmt_resolution_hz;    /*!< RMT resolution in Hz (default: 10MHz) */
} led_strip_config_t;

/**
 * @brief LED strip handle
 */
typedef struct led_strip_t* led_strip_handle_t;

/**
 * @brief Create and initialize an LED strip
 *
 * @param config Pointer to LED strip configuration
 * @param handle Pointer to store the LED strip handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_strip_create(const led_strip_config_t *config, led_strip_handle_t *handle);

/**
 * @brief Delete an LED strip and free resources
 *
 * @param handle LED strip handle to delete
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_strip_delete(led_strip_handle_t handle);

/**
 * @brief Set RGB color for a specific LED (for RGB strips)
 *
 * @param handle LED strip handle
 * @param led_index Index of the LED (0-based)
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_strip_set_pixel_rgb(led_strip_handle_t handle, uint16_t led_index,
                                   uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Set RGBW color for a specific LED (for RGBW strips)
 *
 * @param handle LED strip handle
 * @param led_index Index of the LED (0-based)
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @param white White component (0-255)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_strip_set_pixel_rgbw(led_strip_handle_t handle, uint16_t led_index,
                                    uint8_t red, uint8_t green, uint8_t blue, uint8_t white);

/**
 * @brief Refresh the LED strip to show the updated colors
 *
 * This function sends the buffered color data to the LEDs via RMT.
 * Call this after setting pixel colors to make them visible.
 *
 * @param handle LED strip handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_strip_refresh(led_strip_handle_t handle);

/**
 * @brief Clear all LEDs (turn off)
 *
 * @param handle LED strip handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_strip_clear(led_strip_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // LED_DRIVER_H
