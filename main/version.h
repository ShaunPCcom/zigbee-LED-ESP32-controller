/**
 * @file version.h
 * @brief Firmware version constants (C/C++ compatible)
 *
 * Single source of truth for firmware version numbers.
 * Used by OTA subsystem, logging, and release workflows.
 *
 * CRITICAL: Update FW_VERSION_* values when creating new releases.
 * All other constants derive automatically from these three numbers.
 * The release workflow validates these match the git tag.
 */

#ifndef VERSION_H
#define VERSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * SINGLE SOURCE OF TRUTH - Update only these three values for new releases
 * ============================================================================ */
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 4

/* ============================================================================
 * DERIVED CONSTANTS - Do not modify, these are generated automatically
 * ============================================================================ */

/* String helper macros for version construction */
#define _FW_STR(x) #x
#define FW_STR(x) _FW_STR(x)

/**
 * Firmware version for OTA updates (derived from MAJOR.MINOR.PATCH)
 *
 * Format: 0x00MMNNPP where MM=major, NN=minor, PP=patch
 * - v1.0.1 = 0x00010001
 * - v1.1.3 = 0x00010103
 */
#define FIRMWARE_VERSION \
    ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)

/**
 * Firmware version as string for logging (derived, includes 'v' prefix)
 *
 * Used in log messages to display human-readable version.
 * Format: "vM.m.p" (e.g., "v1.1.3")
 */
#define FIRMWARE_VERSION_STRING \
    "v" FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)

/**
 * Software build ID for Zigbee Basic cluster (ZCL CHAR_STRING format)
 *
 * Used by Z2M to display firmware version in device info.
 * Format: length byte + version string (without 'v' prefix)
 *
 * NOTE: Length byte calculation:
 *   - Single-digit version (1.1.3): len = 1 + 1 + 1 + 2 = 5 chars
 *   - Double-digit minor (1.10.3): len = 1 + 2 + 1 + 2 = 6 chars
 *   - Verify length matches actual string after changing version numbers!
 *
 * For v1.1.4: "1.1.4" = 5 chars = 0x05
 */
#define FIRMWARE_SW_BUILD_ID \
    "\x05" FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)

#ifdef __cplusplus
}
#endif

#endif // VERSION_H
