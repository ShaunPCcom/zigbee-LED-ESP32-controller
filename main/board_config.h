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

/* LED strip GPIOs (SPI2 MOSI, time-multiplexed) */
#define LED_STRIP_1_GPIO               4
#define LED_STRIP_2_GPIO               5

/* Max physical strips */
#define MAX_STRIPS                     2

/* Per-strip LED count defaults */
#define LED_STRIP_1_COUNT              30   /* Strip 1 default LED count */
#define LED_STRIP_2_COUNT              0    /* Strip 2 default: disabled */

/* Segment configuration */
#define MAX_SEGMENTS                   8    /* Maximum number of virtual segments */
#define ZB_SEGMENT_EP_BASE             1    /* EP1 = segment 0, EP2 = segment 1, ... */

/* Default power-on behavior (ZCL StartUpOnOff attr 0x4003)
 * 0x00 = off, 0x01 = on, 0x02 = toggle, 0xFF = previous */
#define DEFAULT_STARTUP_ON_OFF         0xFF /* restore previous state */

#ifdef __cplusplus
}
#endif

#endif // BOARD_CONFIG_H
