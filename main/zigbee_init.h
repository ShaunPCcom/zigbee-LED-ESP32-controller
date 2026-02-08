/**
 * @file zigbee_init.h
 * @brief Zigbee stack initialization and device configuration
 *
 * This module handles the initialization of the Zigbee stack and defines
 * the device as a Color Dimmable Light (Zigbee Router).
 */

#ifndef ZIGBEE_INIT_H
#define ZIGBEE_INIT_H

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zigbee device configuration
 */
#define ZB_DEVICE_MANUFACTURER_NAME     "DIY"
#define ZB_DEVICE_MODEL_IDENTIFIER      "ZB_LED_CTRL"
#define ZB_DEVICE_SW_VERSION            1

/**
 * @brief Zigbee endpoint definitions
 * Endpoint 1: RGB LED strip (Color Dimmable Light)
 * Endpoint 2: White channel (Dimmable Light)
 */
#define ZB_LED_ENDPOINT                 1
#define ZB_WHITE_ENDPOINT               2

/**
 * @brief Initialize Zigbee stack and create device
 *
 * Creates a Zigbee Router device with Color Dimmable Light profile.
 * Registers clusters: Basic, Identify, On/Off, Level Control, Color Control
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t zigbee_init(void);

/**
 * @brief Start Zigbee network steering (join network)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t zigbee_start(void);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_INIT_H
