/**
 * @file board_config.h
 * @brief Board-specific pin and configuration definitions
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO definitions for ESP32-H2-DevKitM-1 */
#define BOARD_LED_GPIO                 8      /* Onboard WS2812 LED */
#define BOARD_BUTTON_GPIO              9      /* Boot button */

/* Button hold times (milliseconds) */
#define BOARD_BUTTON_HOLD_ZIGBEE_MS    3000   /* Zigbee network reset */
#define BOARD_BUTTON_HOLD_FULL_MS      10000  /* Full factory reset (Zigbee + NVS) */

/* LED strip GPIOs */
#define LED_STRIP_1_GPIO               4
#define LED_STRIP_2_GPIO               5
#define LED_STRIP_3_GPIO               10

#ifdef __cplusplus
}
#endif

#endif // BOARD_CONFIG_H
