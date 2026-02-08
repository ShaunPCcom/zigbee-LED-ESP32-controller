/**
 * @file led_cli.h
 * @brief Serial CLI for LED controller configuration
 */

#ifndef LED_CLI_H
#define LED_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install UART driver and start CLI task
 *
 * Commands prefixed with "led ":
 *   led help
 *   led count <n>          - set LED count (saves to NVS, reboot to apply)
 *   led config             - show current config
 *   led nvs                - NVS health check
 *   led reboot             - restart device
 *   led repair             - Zigbee network reset (re-pair)
 *   led factory-reset      - full factory reset (Zigbee + NVS)
 */
void led_cli_start(void);

#ifdef __cplusplus
}
#endif

#endif // LED_CLI_H
