/**
 * @file board_led.h
 * @brief Onboard LED status indicators
 *
 * Controls the onboard WS2812 LED (GPIO8) to show device status:
 * - Not joined: Slow blink
 * - Pairing: Fast blink
 * - Joined: Solid on
 * - Error: Rapid blink
 */

#ifndef BOARD_LED_H
#define BOARD_LED_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED states
 */
typedef enum {
    BOARD_LED_OFF,           // LED off
    BOARD_LED_NOT_JOINED,    // Slow blink - not connected to network
    BOARD_LED_PAIRING,       // Fast blink - pairing mode
    BOARD_LED_JOINED,        // Solid - connected to network
    BOARD_LED_ERROR,         // Very fast blink - error state
} board_led_state_t;

/**
 * @brief Initialize board LED
 */
void board_led_init(void);

/**
 * @brief Set LED state
 */
void board_led_set_state(board_led_state_t state);

/**
 * @brief Update LED (call from button task to handle blinking)
 */
void board_led_update(void);

#ifdef __cplusplus
}
#endif

#endif // BOARD_LED_H
