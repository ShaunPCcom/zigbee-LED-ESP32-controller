/**
 * @file version.h
 * @brief Firmware version constants (C/C++ compatible)
 *
 * Single source of truth for firmware version numbers.
 * Used by OTA subsystem, logging, and release workflows.
 *
 * CRITICAL: Update these values when creating new releases.
 * The release workflow validates these match the git tag.
 */

#ifndef VERSION_H
#define VERSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Firmware version for OTA updates
 *
 * Format: 0x00MMNNBB where MM=major, NN=minor, BB=build
 * - v1.1.1 = 0x00010100
 * - Major version change: Incompatible API/behavior changes
 * - Minor version change: New features, backward compatible
 * - Build number: Bug fixes, patches
 */
#define FIRMWARE_VERSION 0x00010200

/**
 * Firmware version as string for logging
 *
 * Used in log messages to display human-readable version.
 * Keep in sync with FIRMWARE_VERSION above.
 */
#define FIRMWARE_VERSION_STRING "v1.1.2"

#ifdef __cplusplus
}
#endif

#endif // VERSION_H
