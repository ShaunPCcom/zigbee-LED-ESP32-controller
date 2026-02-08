/**
 * @file zigbee_init.h
 * @brief Zigbee stack initialization and device configuration
 *
 * Device: 8 x Extended Color Light endpoints (EP1-EP8, one per segment).
 * Color mode HS/XY = RGB channels; CT mode = White channel.
 * Segment 1 (EP1) also hosts the device and segment config custom clusters.
 */

#ifndef ZIGBEE_INIT_H
#define ZIGBEE_INIT_H

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device identification */
#define ZB_DEVICE_MANUFACTURER_NAME     "DIY"
#define ZB_DEVICE_MODEL_IDENTIFIER      "ZB_LED_CTRL"
#define ZB_DEVICE_SW_VERSION            1

/**
 * @brief Custom cluster 0xFC00: Device configuration
 *   0x0000: led_count   (U16) — strip 0 count, backwards-compat alias
 *   0x0001: strip1_count (U16) — strip 0 LED count
 *   0x0002: strip2_count (U16) — strip 1 LED count
 */
#define ZB_CLUSTER_DEVICE_CONFIG        0xFC00
#define ZB_ATTR_LED_COUNT               0x0000
#define ZB_ATTR_STRIP1_COUNT            0x0001
#define ZB_ATTR_STRIP2_COUNT            0x0002

/**
 * @brief Custom cluster 0xFC01: Segment geometry
 *   For segment N (0-7): base + N*3 + 0 = start, +1 = count, +2 = strip (1-indexed)
 */
#define ZB_CLUSTER_SEGMENT_CONFIG       0xFC01
#define ZB_ATTR_SEG_BASE                0x0000
#define ZB_SEG_ATTRS_PER_SEG            3

/**
 * @brief Initialize Zigbee stack and create device
 */
esp_err_t zigbee_init(void);

/**
 * @brief Start Zigbee network steering (join network)
 */
esp_err_t zigbee_start(void);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_INIT_H
