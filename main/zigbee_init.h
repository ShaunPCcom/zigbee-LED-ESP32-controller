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
 *   0x0000: led_count            (U16, RW) — strip 0 count, backwards-compat alias
 *   0x0001: strip1_count         (U16, RW) — strip 0 LED count
 *   0x0002: strip2_count         (U16, RW) — strip 1 LED count
 *   0x0003: global_transition_ms (U16, RW) — default transition duration in ms
 */
#define ZB_CLUSTER_DEVICE_CONFIG        0xFC00
#define ZB_ATTR_LED_COUNT               0x0000
#define ZB_ATTR_STRIP1_COUNT            0x0001
#define ZB_ATTR_STRIP2_COUNT            0x0002
#define ZB_ATTR_GLOBAL_TRANSITION_MS    0x0003

/**
 * @brief Custom cluster 0xFC01: Segment geometry
 *   For segment N (0-7): base + N*3 + 0 = start, +1 = count, +2 = strip (1-indexed)
 */
#define ZB_CLUSTER_SEGMENT_CONFIG       0xFC01
#define ZB_ATTR_SEG_BASE                0x0000
#define ZB_SEG_ATTRS_PER_SEG            3

/**
 * @brief Custom cluster 0xFC02: Preset configuration
 *   0x0000: preset_count    (U8) — number of stored presets (0-8)
 *   0x0001: active_preset   (CharString) — DEPRECATED: name of last recalled preset
 *   0x0002: recall_preset   (CharString) — DEPRECATED: write name to recall preset
 *   0x0003: save_preset     (CharString) — DEPRECATED: write name to save preset
 *   0x0004: delete_preset   (CharString) — DEPRECATED: write name to delete preset
 *   0x0010-0x0017: preset_N_name (CharString) — names of stored presets (slots 0-7)
 *   0x0020: recall_slot     (U8, RW) — write slot 0-7 to recall preset
 *   0x0021: save_slot       (U8, RW) — write slot 0-7 to save current state
 *   0x0022: delete_slot     (U8, RW) — write slot 0-7 to delete preset
 *   0x0023: save_name       (CharString, RW) — name for next save operation
 */
#define ZB_CLUSTER_PRESET_CONFIG        0xFC02
#define ZB_ATTR_PRESET_COUNT            0x0000
#define ZB_ATTR_ACTIVE_PRESET           0x0001  /* DEPRECATED */
#define ZB_ATTR_RECALL_PRESET           0x0002  /* DEPRECATED */
#define ZB_ATTR_SAVE_PRESET             0x0003  /* DEPRECATED */
#define ZB_ATTR_DELETE_PRESET           0x0004  /* DEPRECATED */
#define ZB_ATTR_PRESET_NAME_BASE        0x0010
#define ZB_ATTR_RECALL_SLOT             0x0020
#define ZB_ATTR_SAVE_SLOT               0x0021
#define ZB_ATTR_DELETE_SLOT             0x0022
#define ZB_ATTR_SAVE_NAME               0x0023

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
