#pragma once

#include <stdint.h>  // uint8_t, uint16_t, uint32_t types

/**
 * @file project_defaults.hpp
 * @brief Project-wide default configuration values for Zigbee LED Controller
 *
 * PURPOSE:
 * This file centralizes ALL built-in default values, constants, and configuration
 * parameters for the LED controller project. It replaces scattered magic numbers
 * across the codebase with well-documented, single-source-of-truth values.
 *
 * RULES FOR USE:
 * 1. This is the ONLY place default values should live - no magic numbers in source files
 * 2. All project code references defaults:: namespace for their default values
 * 3. This file replaces board_config.h for GPIO pins and hardware configuration
 * 4. Shared components (transition_engine, etc.) receive defaults as constructor/init
 *    parameters - they don't include this file directly to avoid circular dependencies
 * 5. When adding a new constant:
 *    - Place it in the appropriate logical section
 *    - Write a clear comment explaining WHAT it is, WHY this value, and any context
 *    - Use descriptive names (not abbreviations unless industry-standard)
 *
 * USAGE EXAMPLES:
 * - In project code:
 *     gpio_set_direction(defaults::LED_STRIP_1_GPIO, GPIO_MODE_OUTPUT);
 *     transition_engine_init(defaults::TRANSITION_ENGINE_UPDATE_RATE_HZ);
 *
 * - Passing to shared components:
 *     BoardLed led(defaults::BOARD_LED_GPIO);
 *     segment_manager_init(defaults::LED_STRIP_1_COUNT);
 *
 * - NVS default values:
 *     if (nvs_read_failed) {
 *         transition_ms = defaults::GLOBAL_TRANSITION_TIME_MS;
 *     }
 *
 * WHAT SHOULD NOT GO HERE:
 * - Runtime variables (those belong in their respective modules)
 * - Zigbee stack constants from esp-zigbee-sdk (use SDK headers)
 * - Hardware capabilities determined at runtime (flash size, chip revision)
 * - Values that must be calculated based on other values (derived constants)
 */

namespace defaults {

// ============================================================================
// Hardware Configuration - ESP32-H2 GPIO Pin Assignments
// ============================================================================

/**
 * GPIO for LED Strip 1 data line (primary strip)
 *
 * Physically connected to SPI2 MOSI signal. ESP32-H2 uses SPI time-multiplexing
 * for dual strips: single SPI2 peripheral with MOSI GPIO remapped between
 * strip transmissions via GPIO matrix routing.
 *
 * Technical: GPIO4 is SPI2 MOSI default, chosen to minimize routing conflicts.
 * SK6812 RGBW strips require precise 3.2MHz-equivalent timing (achieved via
 * 2.5MHz SPI clock with 3-bit encoding: 0->100b, 1->110b).
 */
constexpr uint8_t LED_STRIP_1_GPIO = 4;

/**
 * GPIO for LED Strip 2 data line (secondary strip, time-multiplexed)
 *
 * Also connected to SPI2 MOSI via GPIO matrix remapping. Between strip refreshes,
 * firmware calls esp_rom_gpio_connect_out_signal() to switch the MOSI output
 * from GPIO4 to GPIO5, enabling dual physical strips with single SPI peripheral.
 *
 * Why time-mux: ESP32-H2 RMT peripheral has limited TX channels (only 1 safe
 * with Zigbee stack active). SPI approach is robust and timing-predictable.
 *
 * Note: Strip 2 defaults to LED_STRIP_2_COUNT=0 (disabled) until user configures.
 */
constexpr uint8_t LED_STRIP_2_GPIO = 5;

/**
 * GPIO for onboard status LED (ESP32-H2-DevKitM-1 built-in WS2812)
 *
 * Single RGB LED used for device status indication, driven via RMT TX channel 0.
 * Independent of main LED strips (which use SPI). Status colors:
 *   - Amber: Device not joined to Zigbee network
 *   - Blue: Pairing mode active
 *   - Green: Successfully joined (5 seconds, then off)
 *   - Red: Error condition (5 seconds, then back to pairing mode)
 *
 * Why RMT for board LED: Onboard LED shares Strip 1 pixels 0-2 physically,
 * but logically separate. RMT timing is more precise for single LED, while
 * SPI bulk-transmits entire strip buffers efficiently.
 */
constexpr uint8_t BOARD_LED_GPIO = 8;

/**
 * GPIO for boot/user button (ESP32-H2-DevKitM-1 built-in)
 *
 * Multi-function button with hold-time detection for factory reset operations:
 *   - 3 second hold: Zigbee network reset (leave network, keep config)
 *   - 10 second hold: Full factory reset (Zigbee + NVS erase, all settings lost)
 *
 * Technical: Active-low with internal pull-up. Hold times defined in
 * BOARD_BUTTON_HOLD_* constants below.
 */
constexpr uint8_t BOARD_BUTTON_GPIO = 9;

/**
 * Maximum number of physical LED strips supported by hardware
 *
 * Current hardware configuration: 2 strips via SPI2 time-multiplexing.
 * Theoretical limit: Could support more strips by adding additional GPIOs
 * and cycling through them, but 2 is practical max for ESP32-H2 (limited
 * available GPIOs after Zigbee radio requirements).
 *
 * Used by: led_driver.c array sizing, config_storage validation
 */
constexpr uint8_t MAX_PHYSICAL_STRIPS = 2;

/**
 * Maximum virtual segments across all physical strips
 *
 * Each segment becomes a Zigbee Extended Color Light endpoint (EP1-EP8).
 * Segments are software divisions of physical strips - multiple segments can
 * overlay the same physical LEDs, controlled independently via Zigbee.
 *
 * Why 8: Balance between flexibility (fine-grained control) and Zigbee endpoint
 * limits. Each segment consumes ZCL attribute memory and network bandwidth.
 * 8 segments = 8 independent Home Assistant light entities.
 *
 * Used by: segment_manager state arrays, Zigbee endpoint creation, Z2M converter
 */
constexpr uint8_t MAX_SEGMENTS = 8;

// ============================================================================
// LED Hardware Specifications
// ============================================================================

/**
 * Default LED count for Strip 1 (primary strip)
 *
 * Initial value when NVS is empty or after factory reset. User can reconfigure
 * via Zigbee attribute 0xFC00:0x0001 (strip1_count) or CLI command.
 *
 * Why 30: Common retail WS2812/SK6812 strip length (1 meter at 30 LEDs/m).
 * Fits typical desk/room accent lighting without overwhelming ESP32-H2 memory
 * (each LED = 4 bytes GRBW = 120 bytes for 30 LEDs).
 *
 * Maximum safe: ~150 LEDs per strip (600 bytes GRBW) before SPI DMA buffer
 * size becomes concern. Total project tested up to 60 LEDs (2x30) reliably.
 */
constexpr uint16_t LED_STRIP_1_COUNT = 30;

/**
 * Default LED count for Strip 2 (secondary strip, disabled by default)
 *
 * Value of 0 means strip disabled. When disabled:
 *   - GPIO5 driven low (idle state)
 *   - No SPI transmission to strip 2
 *   - No memory allocated for strip 2 pixel buffer
 *   - Segments default to strip_id=0 (all on Strip 1)
 *
 * Enable Strip 2: Set via Zigbee attribute 0xFC00:0x0002 or CLI, then reboot.
 * After enabling, segments can be assigned to either physical strip via
 * segment geometry configuration (strip_id field).
 */
constexpr uint16_t LED_STRIP_2_COUNT = 0;

/**
 * Number of color bytes per LED pixel (SK6812 RGBW format)
 *
 * SK6812 RGBW LEDs use 4-byte format: [G, R, B, W] (green-first byte order).
 * This differs from WS2812B which is RGB-only (3 bytes: [G, R, B]).
 *
 * Why GRBW: Industry standard for addressable RGBW strips. Green-first
 * optimizes for human eye sensitivity (green most visible). White channel
 * provides pure whites for color temperature (CT) mode without RGB mixing.
 *
 * Used by: led_driver.c pixel buffer sizing, color conversion functions
 */
constexpr uint8_t BYTES_PER_LED_PIXEL = 4;

/**
 * SPI clock frequency for LED data encoding (2.5 MHz)
 *
 * Chosen to produce 400ns-per-bit timing required by SK6812 protocol:
 *   - 2.5 MHz = 400ns period per SPI bit
 *   - Each LED bit encoded as 3 SPI bits (0 -> 100b, 1 -> 110b)
 *   - Effective data rate: 2.5MHz / 3 = 833 kHz LED protocol rate
 *
 * Why not faster: SK6812 timing spec is strict (±150ns tolerance).
 * 2.5MHz SPI produces exact 400ns, well within tolerance. Higher speeds
 * risk timing violations and LED protocol errors (flickering, wrong colors).
 *
 * Reference: SK6812 datasheet T0H=300ns, T0L=900ns (0 bit), T1H=600ns, T1L=600ns (1 bit)
 */
constexpr uint32_t LED_SPI_CLOCK_HZ = 2500000;

/**
 * SPI bytes per LED byte (3-bit encoding overhead)
 *
 * Each LED color byte (8 bits) becomes 24 SPI bits (3 bytes) via encoding:
 *   - LED bit 0 -> SPI bits 100 (binary)
 *   - LED bit 1 -> SPI bits 110 (binary)
 *
 * Example: LED byte 0b10110100 ->
 *   SPI bytes: [110][100][110][110][100][110][100][100] = 3 bytes
 *
 * Math: 8 LED bits × 3 SPI bits per LED bit = 24 SPI bits = 3 bytes
 */
constexpr uint8_t SPI_BYTES_PER_LED_BYTE = 3;

/**
 * SPI bytes for LED reset/latch signal (40 bytes = 128us low)
 *
 * SK6812 protocol requires >80us low signal to latch new data. Achieved by
 * transmitting 40 bytes of 0x00 after pixel data:
 *   - 40 bytes × 8 bits × 400ns = 128us (well above 80us minimum)
 *
 * Why 40: Conservative safety margin. 25 bytes (80us) would be minimum, but
 * SPI timing jitter and bus contention could reduce effective low time.
 * 40 bytes = 60% safety margin, negligible performance cost (0.128ms per refresh).
 *
 * Used by: led_driver.c SPI buffer sizing, appended after pixel data
 */
constexpr uint8_t LED_RESET_BYTES = 40;

// ============================================================================
// Zigbee Configuration
// ============================================================================

/**
 * Base Zigbee endpoint number for segment endpoints
 *
 * Segments map to endpoints sequentially:
 *   - Segment 0 (index 0) = Endpoint 1
 *   - Segment 1 (index 1) = Endpoint 2
 *   - ...
 *   - Segment 7 (index 7) = Endpoint 8
 *
 * Why start at 1: Endpoint 0 is reserved by Zigbee spec for ZDO (Zigbee
 * Device Object) management. Application endpoints must be 1-240.
 *
 * EP1 special role: Hosts custom clusters (0xFC00, 0xFC01, 0xFC02) in addition
 * to standard Extended Color Light clusters. All other EPs are light-only.
 */
constexpr uint8_t ZB_SEGMENT_EP_BASE = 1;

/**
 * Zigbee device type ID (Extended Color Light)
 *
 * Value 0x0210 = Extended Color Light per Zigbee Home Automation profile.
 * Supports:
 *   - On/Off cluster (0x0006)
 *   - Level Control cluster (0x0008) - brightness
 *   - Color Control cluster (0x0300) - HS, XY, and CT modes
 *
 * Why Extended vs Color Light: Regular Color Light (0x0200) lacks color
 * temperature (CT) support. Extended variant includes all three color modes:
 *   - HS (Hue/Saturation) for RGB colors
 *   - XY (CIE color space) for precise color (unused in this project)
 *   - CT (Color Temperature) for white channel, measured in mireds
 *
 * Note: ESP-IDF Zigbee SDK doesn't define ESP_ZB_HA_EXTENDED_COLOR_LIGHT_DEVICE_ID,
 * so raw hex value used.
 */
constexpr uint16_t ZB_DEVICE_TYPE_EXTENDED_COLOR_LIGHT = 0x0210;

/**
 * Zigbee manufacturer code (Espressif)
 *
 * Registered code for Espressif Systems: 0x131B (4891 decimal)
 * Used in:
 *   - OTA firmware updates (manufacturer_code field in OTA cluster)
 *   - Device identification
 *
 * Why Espressif code: This project uses ESP32-H2 and ESP-IDF framework.
 * Using Espressif's official code ensures compatibility with their OTA
 * infrastructure and Z2M device database.
 */
constexpr uint16_t ZB_MANUFACTURER_CODE_ESPRESSIF = 0x131B;

/**
 * OTA image type identifier (LED Controller)
 *
 * Distinguishes LED controller firmware from other devices sharing the same
 * manufacturer code. Each device type needs unique image_type to prevent
 * cross-device firmware updates (e.g., LD2450 sensor receiving LED firmware).
 *
 * Why 0x0002: Arbitrary assignment within Espressif namespace.
 *   - 0x0001 used by LD2450 Zigbee sensor (separate project)
 *   - 0x0002 assigned to this LED controller
 *
 * Used by: OTA cluster configuration, firmware file naming, Z2M OTA index
 */
constexpr uint16_t ZB_OTA_IMAGE_TYPE_LED_CONTROLLER = 0x0002;

/**
 * OTA query interval (check for updates every 24 hours)
 *
 * Device asks coordinator for firmware updates at this interval.
 * 1440 minutes = 24 hours = reasonable balance between:
 *   - Timely updates (users see new firmware within a day)
 *   - Low network overhead (one query per day negligible)
 *   - Battery life (not applicable for mains-powered device)
 *
 * Note: Manual update checks always possible via Z2M UI regardless of interval.
 */
constexpr uint16_t ZB_OTA_QUERY_INTERVAL_MINUTES = 1440;

// ============================================================================
// Zigbee Custom Cluster IDs (Manufacturer-Specific)
// ============================================================================

/**
 * Custom cluster for device-wide configuration (0xFC00)
 *
 * Manufacturer-specific cluster on EP1 for global device settings:
 *   - Attribute 0x0000: led_count (deprecated alias for strip1_count)
 *   - Attribute 0x0001: strip1_count (U16, LED count for physical strip 0)
 *   - Attribute 0x0002: strip2_count (U16, LED count for physical strip 1)
 *   - Attribute 0x0003: global_transition_ms (U16, default fade duration)
 *
 * Why 0xFC00: Range 0xFC00-0xFFFF reserved for manufacturer-specific clusters
 * per Zigbee spec. 0xFC00 chosen as first available, simple to remember.
 */
constexpr uint16_t ZB_CLUSTER_DEVICE_CONFIG = 0xFC00;

/**
 * Custom cluster for segment geometry configuration (0xFC01)
 *
 * Defines how 8 virtual segments map to physical LED positions.
 * Per segment (N = 0-7), three consecutive attributes:
 *   - Base + N×3 + 0: start (U16) - first LED index in physical strip
 *   - Base + N×3 + 1: count (U16) - number of LEDs in segment (0 = disabled)
 *   - Base + N×3 + 2: strip_id (U8) - physical strip index (1-indexed: 1 or 2)
 *
 * Why separate cluster: Keeps geometry config isolated from device config.
 * 24 attributes (8 segments × 3) would clutter 0xFC00.
 */
constexpr uint16_t ZB_CLUSTER_SEGMENT_CONFIG = 0xFC01;

/**
 * Custom cluster for preset save/recall (0xFC02)
 *
 * Enables saving and recalling all 8 segment states as named presets for
 * Home Assistant automations. Includes both slot-based (current) and
 * name-based (deprecated) attribute interfaces.
 *
 * Key attributes:
 *   - 0x0000: preset_count (U8, read-only) - number of occupied slots
 *   - 0x0010-0x0017: preset_N_name (CharString, read-only) - slot names
 *   - 0x0020: recall_slot (U8, write) - trigger recall by slot number
 *   - 0x0021: save_slot (U8, write) - save current state to slot
 *   - 0x0022: delete_slot (U8, write) - erase preset from slot
 *   - 0x0023: save_name (CharString, write) - name for next save operation
 *
 * Design: Slot-based (not name-based) for stable HA automation references.
 */
constexpr uint16_t ZB_CLUSTER_PRESET_CONFIG = 0xFC02;

// ============================================================================
// Timing and Performance
// ============================================================================

/**
 * Transition engine update rate (200 Hz)
 *
 * Periodic timer tick rate for smooth brightness/color interpolation.
 * 200 Hz chosen to match commercial smart bulb refresh rates:
 *   - 200 Hz = 5ms update interval
 *   - Human eye perceives <100Hz as flicker, >150Hz as smooth
 *   - 200 Hz provides imperceptible steps during 1-5 second transitions
 *
 * Performance: 200Hz × 32 transitions (4 per segment × 8 segments) = 6400
 * calculations/sec. Negligible CPU load on 96MHz ESP32-H2 (~0.1% utilization).
 *
 * Trade-off: Higher rates (500Hz) smoother but waste CPU. Lower rates (50Hz)
 * save power but visible stepping during slow fades.
 */
constexpr uint16_t TRANSITION_ENGINE_UPDATE_RATE_HZ = 200;

/**
 * Default global transition time (1000 milliseconds = 1 second)
 *
 * When user doesn't specify transition duration in Zigbee command, this value
 * applies. Used for:
 *   - Brightness changes (dim/brighten)
 *   - Color changes (hue/saturation shifts)
 *   - Color temperature changes (warm/cool white)
 *
 * Why 1 second: Industry standard for smart lighting. Fast enough for
 * responsive UI, slow enough to appear deliberate (not jarring). User can
 * override per-command or change global default via Zigbee attribute
 * 0xFC00:0x0003 or CLI.
 *
 * Special cases:
 *   - On/Off: Always instant (0ms), ignores global default
 *   - Preset recall: Uses global default unless override provided
 *
 * Range: 0-65535ms supported by Zigbee U16 attribute type
 */
constexpr uint16_t GLOBAL_TRANSITION_TIME_MS = 1000;

/**
 * NVS save debounce delay (500 milliseconds)
 *
 * After state change, firmware waits 500ms before writing to NVS flash.
 * If another change occurs during wait, timer resets. This debouncing:
 *   - Reduces flash wear (NVS has ~100k write cycle limit per cell)
 *   - Improves performance (batch multiple rapid changes into one write)
 *   - Avoids blocking Zigbee stack during intensive flash operations
 *
 * Example: User slides brightness 0->254 in Home Assistant. Without debounce,
 * firmware would write 254 NVS updates. With debounce, only final value saved.
 *
 * Why 500ms: Long enough to batch rapid UI interactions (human reaction time
 * ~200ms), short enough to capture state before unexpected power loss.
 */
constexpr uint32_t NVS_SAVE_DEBOUNCE_MS = 500;

// ============================================================================
// Button Configuration
// ============================================================================

/**
 * Button hold duration for Zigbee network reset (3 seconds)
 *
 * Hold boot button for 3 seconds to trigger Zigbee network leave operation.
 * Clears network credentials and steering state, but preserves:
 *   - LED counts and segment geometry (NVS config)
 *   - Saved presets
 *   - Global transition time
 *
 * After reset, device enters pairing mode (blue status LED) waiting for
 * coordinator to permit join.
 *
 * Why 3 seconds: Long enough to prevent accidental activation (requires
 * deliberate action), short enough for user convenience. Industry standard
 * for Zigbee device reset (matches Philips Hue, IKEA Tradfri patterns).
 */
constexpr uint32_t BOARD_BUTTON_HOLD_ZIGBEE_MS = 3000;

/**
 * Button hold duration for full factory reset (10 seconds)
 *
 * Hold boot button for 10 seconds to trigger complete device reset:
 *   - Zigbee network leave (same as 3s hold)
 *   - NVS flash erase (all saved settings lost)
 *   - Returns to factory defaults:
 *     - Strip counts: 30 LEDs (strip 1), 0 LEDs (strip 2)
 *     - Segment 1: Full strip (start=0, count=30)
 *     - Segments 2-8: Disabled (count=0)
 *     - All presets: Erased
 *     - Global transition: 1000ms
 *
 * Why 10 seconds: Extremely long hold prevents accidental data loss. User
 * must WANT this destructive operation. Comparable to long-press factory
 * reset on consumer routers (often 10-30 seconds).
 *
 * Use case: Preparing device for resale, troubleshooting corrupted NVS
 */
constexpr uint32_t BOARD_BUTTON_HOLD_FULL_MS = 10000;

/**
 * Button polling interval (50 milliseconds)
 *
 * FreeRTOS task wakes every 50ms to check button state for hold detection.
 *
 * Why 50ms: Balances responsiveness vs CPU efficiency. Human can't detect
 * <100ms latency in button response. 50ms = 20Hz poll rate, trivial CPU
 * load on dedicated task.
 *
 * Debouncing: Mechanical switches bounce 5-20ms. 50ms interval naturally
 * filters bounce without extra logic (first stable read after bounce wins).
 */
constexpr uint32_t BUTTON_POLL_INTERVAL_MS = 50;

// ============================================================================
// Color and Light Defaults
// ============================================================================

/**
 * Default startup behavior (restore previous state)
 *
 * Zigbee StartUpOnOff attribute (0x4003) value applied at boot:
 *   - 0x00: Always off after power-on
 *   - 0x01: Always on after power-on
 *   - 0x02: Toggle (opposite of last state)
 *   - 0xFF: Restore previous state (on/off remembered)
 *
 * Why 0xFF: Most intuitive for smart lighting. Power outage shouldn't change
 * user's lighting scene. Matches commercial bulbs (Hue, LIFX restore state).
 *
 * Per-segment: Each segment stores its own startup_on_off value. This is the
 * default applied when segment created or NVS empty.
 */
constexpr uint8_t DEFAULT_STARTUP_ON_OFF = 0xFF;

/**
 * Default brightness level (50% = 128/254)
 *
 * Applied to new segments when NVS empty or factory reset.
 * Zigbee Level Control uses range 0-254 (not 0-255, to avoid confusion with
 * null/invalid 0xFF marker in ZCL).
 *
 * Why 50%: Safe middle ground. Not blindingly bright (user might have light
 * pointed at eyes), not too dim (visually confirms device working).
 * Half brightness also extends LED lifetime.
 */
constexpr uint8_t DEFAULT_BRIGHTNESS_LEVEL = 128;

/**
 * Default color temperature (250 mireds = ~4000K neutral white)
 *
 * Mireds = million reciprocal degrees. Conversion: mireds = 1,000,000 / Kelvin
 *   - 250 mireds = 1M / 250 = 4000K (neutral/cool white, office lighting)
 *   - 153 mireds = 6500K (cool daylight, ZCL physical min)
 *   - 370 mireds = 2700K (warm white, incandescent bulb)
 *
 * Why 4000K: Neutral color suitable for any environment. Not too warm (yellowish),
 * not too cool (bluish). Matches typical LED "daylight" bulbs.
 *
 * Range: Cluster reports 153-370 mireds (2700K-6500K), standard for RGBW strips.
 */
constexpr uint16_t DEFAULT_COLOR_TEMP_MIREDS = 250;

/**
 * Minimum color temperature (153 mireds = 6500K cool daylight)
 *
 * ZCL ColorTempPhysicalMinMireds attribute. Hardware limit imposed by white
 * LED phosphor characteristics. Below 6500K, light becomes unnaturally blue.
 */
constexpr uint16_t COLOR_TEMP_MIN_MIREDS = 153;

/**
 * Maximum color temperature (370 mireds = 2700K warm incandescent)
 *
 * ZCL ColorTempPhysicalMaxMireds attribute. Above 2700K (lower mireds), light
 * becomes dim orange/red, not useful for illumination.
 */
constexpr uint16_t COLOR_TEMP_MAX_MIREDS = 370;

/**
 * Default CIE color space X coordinate (0x616B = ~0.38)
 *
 * ZCL XY color mode uses CIE 1931 color space with U16 fixed-point encoding:
 *   - 0x0000 = 0.0
 *   - 0xFEFF = 0.9961 (max valid, 0xFFFF reserved)
 *   - 0x616B = 24939 / 65535 ≈ 0.38
 *
 * Value (0.38, 0.38) approximates neutral white in CIE space. Not pure white
 * (0.33, 0.33) but close enough for default when XY mode not actively used.
 *
 * Note: This project primarily uses HS (hue/sat) mode for RGB colors and CT
 * mode for whites. XY mode supported for Zigbee compliance but rarely used.
 */
constexpr uint16_t DEFAULT_COLOR_X = 0x616B;

/**
 * Default CIE color space Y coordinate (0x607D = ~0.377)
 *
 * See DEFAULT_COLOR_X comment for encoding details.
 * Pair (0x616B, 0x607D) = approx neutral white in CIE XY space.
 */
constexpr uint16_t DEFAULT_COLOR_Y = 0x607D;

// ============================================================================
// NVS Storage Keys and Namespaces
// ============================================================================

/**
 * NVS namespace for LED configuration ("led_cfg")
 *
 * All device settings stored under this namespace:
 *   - "led_cnt_1": U16, strip 0 LED count
 *   - "led_cnt_2": U16, strip 1 LED count
 *   - "glob_trans": U16, global transition time (ms)
 *   - "seg_geom": blob, segment_geom_t[8] (start/count/strip per segment)
 *   - "seg_state": blob, segment_light_nvs_t[8] (on/level/hue/sat/mode/CT)
 *   - "prst_0" through "prst_7": blob, preset slot data (name + 8 segments)
 *   - "prst_version": U8, preset storage format version (2 = slot-based)
 *
 * Why separate namespace: Isolates LED config from other ESP-IDF components
 * (WiFi, Bluetooth, system). Makes factory reset surgical (erase only led_cfg).
 */
constexpr const char* NVS_NAMESPACE = "led_cfg";

/**
 * NVS key for Strip 1 LED count ("led_cnt_1")
 *
 * Stored as U16. Range: 0-65535 (though practical max ~150 for memory reasons).
 * Loaded at boot before segment manager init.
 */
constexpr const char* NVS_KEY_STRIP1_COUNT = "led_cnt_1";

/**
 * NVS key for Strip 2 LED count ("led_cnt_2")
 *
 * Stored as U16. Value 0 = strip disabled (default).
 */
constexpr const char* NVS_KEY_STRIP2_COUNT = "led_cnt_2";

/**
 * NVS key for global transition time ("glob_trans")
 *
 * Stored as U16, units: milliseconds. Range: 0-65535.
 */
constexpr const char* NVS_KEY_GLOBAL_TRANSITION = "glob_trans";

/**
 * NVS key for segment geometry blob ("seg_geom")
 *
 * Binary blob: 8 × segment_geom_t (currently 5 bytes each = 40 bytes total).
 * Format: [start:U16, count:U16, strip_id:U8] per segment.
 *
 * Version migration: Code checks blob size at load. If size mismatch (struct
 * changed in firmware update), falls back to defaults rather than crash.
 */
constexpr const char* NVS_KEY_SEGMENT_GEOM = "seg_geom";

/**
 * NVS key for segment state blob ("seg_state")
 *
 * Binary blob: 8 × segment_light_nvs_t (14 bytes each = 112 bytes total).
 * Contains ONLY persisted fields (not transition_t runtime state).
 *
 * Format per segment:
 *   - on: bool (1 byte)
 *   - level: U8 (brightness)
 *   - hue: U16 (degrees × 182.04, ZCL encoding)
 *   - saturation: U8
 *   - color_mode: U8 (0=HS, 1=XY, 2=CT)
 *   - color_x: U16 (CIE X coordinate)
 *   - color_y: U16 (CIE Y coordinate)
 *   - color_temp: U16 (mireds)
 *   - startup_on_off: U8 (ZCL StartUpOnOff value)
 *
 * Transition fields (level_trans, hue_trans, etc.) NOT persisted - rebuilt
 * at boot with current_value = loaded state.
 *
 * Migration: v1 format was 12 bytes/segment (no startup_on_off). Loader
 * detects v1 by blob size and migrates to v2 transparently.
 */
constexpr const char* NVS_KEY_SEGMENT_STATE = "seg_state";

/**
 * NVS key prefix for preset slots ("prst_")
 *
 * Full keys: "prst_0", "prst_1", ..., "prst_7" (8 slots)
 * Each blob: 1 + 16 + 112 = 129 bytes
 *   - 1 byte: name_length (0-16)
 *   - 16 bytes: name UTF-8 characters (no null terminator)
 *   - 112 bytes: 8 × segment_light_nvs_t (same format as seg_state)
 *
 * Empty slot: name_length = 0
 */
constexpr const char* NVS_KEY_PRESET_PREFIX = "prst_";

/**
 * NVS key for preset storage version ("prst_version")
 *
 * U8 value indicating preset storage format:
 *   - 0 or missing: Legacy name-based presets (pre-v2)
 *   - 2: Current slot-based presets (Phase 5d refactor)
 *
 * Used for migration: If version < 2, firmware migrates old presets to
 * slot format with default names "Preset 1" through "Preset 8".
 */
constexpr const char* NVS_KEY_PRESET_VERSION = "prst_version";

// ============================================================================
// Preset Configuration
// ============================================================================

/**
 * Maximum number of preset slots (8)
 *
 * Each slot can store all 8 segment states as a named preset. Total capacity:
 * 8 slots × 129 bytes = 1032 bytes NVS usage.
 *
 * Why 8: Balance between flexibility and memory. 8 presets = typical user
 * scenarios (day/night, movie/reading, party, accent, etc.) without overwhelming
 * Home Assistant UI with choices.
 *
 * Zigbee exposure: Slots appear as select entities (dropdown) in HA. Each slot
 * gets read-only name sensor (preset_0_name through preset_7_name).
 */
constexpr uint8_t MAX_PRESET_SLOTS = 8;

/**
 * Maximum preset name length (16 characters, UTF-8)
 *
 * Stored in NVS as variable-length: 1 byte length + up to 16 UTF-8 bytes.
 * No null terminator (length-prefixed, not null-terminated).
 *
 * Why 16: Balance between descriptive names and memory efficiency.
 * Examples that fit:
 *   - "Movie Night" (11 chars)
 *   - "Reading" (7 chars)
 *   - "Party Mode" (10 chars)
 *   - "Relax" (5 chars)
 *
 * Zigbee limitation: CharString attributes have 254 byte max per ZCL spec,
 * but Z2M display truncates long strings. 16 chars fits comfortably in UI.
 */
constexpr uint8_t PRESET_NAME_MAX = 16;

/**
 * Preset storage format version (2 = slot-based)
 *
 * Current version uses numbered slots (0-7) with names as metadata.
 * Previous version (1, implicit) used names as primary identifiers.
 *
 * Version 2 advantages:
 *   - Stable slot references in Home Assistant automations (slot numbers don't change)
 *   - Simpler firmware logic (no string searching)
 *   - Faster NVS lookups (fixed key format "prst_N")
 */
constexpr uint8_t PRESET_VERSION_CURRENT = 2;

// ============================================================================
// Board Status LED (RMT-Driven WS2812)
// ============================================================================

/**
 * RMT peripheral resolution (10 MHz = 100ns per tick)
 *
 * RMT (Remote Control Transceiver) generates precise timing for WS2812 protocol:
 *   - 10MHz clock = 100ns tick resolution
 *   - WS2812 timing: T0H=400ns (4 ticks), T0L=800ns (8 ticks),
 *                    T1H=800ns (8 ticks), T1L=400ns (4 ticks)
 *
 * Why 10MHz: Divides evenly into 80MHz APB clock (ESP32-H2). Provides sufficient
 * resolution for WS2812 timing (±150ns tolerance) without excessive tick counts.
 *
 * Used by: board_led.c RMT configuration, LED encoder timing
 */
constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;

/**
 * Status LED pixel count (3 LEDs: RGB status indicator)
 *
 * Board LED shares physical Strip 1 pixels 0-2 but driven separately via RMT.
 * 3 LEDs used for multi-color status indication:
 *   - Pixel 0: Primary status (amber/blue/green/red)
 *   - Pixels 1-2: Available for future patterns (blink, chase, breathe)
 *
 * Current firmware uses only pixel 0. Pixels 1-2 remain off (RGB=0,0,0).
 */
constexpr uint8_t BOARD_LED_PIXEL_COUNT = 3;

// ============================================================================
// Zigbee Stack Configuration
// ============================================================================

/**
 * Maximum child devices for router role (10)
 *
 * As Zigbee Router (not end device), this controller can relay traffic for
 * other devices. Max children = 10 means up to 10 battery-powered devices
 * can use this controller as their parent for network connectivity.
 *
 * Why 10: Conservative for mains-powered router. ESP32-H2 could handle more
 * (Zigbee spec allows up to 255), but 10 balances mesh reliability vs memory.
 * Typical home has <10 devices per router.
 *
 * Note: Doesn't limit total network size (coordinator handles that). Only
 * limits direct children of THIS device.
 */
constexpr uint8_t ZB_MAX_CHILDREN = 10;

/**
 * Zigbee task stack size (8192 bytes)
 *
 * FreeRTOS task stack for Zigbee main loop. Needs headroom for:
 *   - Zigbee stack internal state (~4KB)
 *   - Callback execution (command handlers, attribute updates)
 *   - esp_zb_scheduler_alarm deferred calls
 *
 * Why 8KB: Espressif recommendation for complex Zigbee devices. Smaller
 * stacks risk overflow during OTA updates or heavy network traffic.
 * Larger stacks waste RAM (ESP32-H2 has only 256KB total).
 */
constexpr uint32_t ZB_TASK_STACK_SIZE = 8192;

/**
 * Zigbee task priority (5, above default task priority 1)
 *
 * Higher priority ensures Zigbee stack responds to network traffic without
 * being starved by lower-priority tasks (LED rendering, CLI, NVS writes).
 *
 * FreeRTOS priority scale: 0 = idle, 1-10 = normal, >10 = real-time
 * Priority 5 = responsive networking without blocking time-critical tasks.
 */
constexpr uint8_t ZB_TASK_PRIORITY = 5;

} // namespace defaults
