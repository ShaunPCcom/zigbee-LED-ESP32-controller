/**
 * @file board_led.h
 * @brief Status LED indication via onboard WS2812 (GPIO8, RMT)
 *
 * Matches LD2450 project indicator behaviour:
 *   NOT_JOINED : amber blink ~2Hz, indefinite
 *   PAIRING    : blue blink ~2Hz, indefinite
 *   JOINED     : solid green for 5 s, then OFF
 *   ERROR      : red blink ~5Hz for 5 s, then NOT_JOINED
 *   OFF        : all dark
 */

#ifndef BOARD_LED_H
#define BOARD_LED_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_LED_OFF = 0,
    BOARD_LED_NOT_JOINED,   /* blinking amber, indefinite */
    BOARD_LED_PAIRING,      /* blinking blue, indefinite */
    BOARD_LED_JOINED,       /* solid green 5 s, then OFF */
    BOARD_LED_ERROR,        /* blinking red 5 s, then NOT_JOINED */
} board_led_state_t;

void board_led_init(void);
void board_led_set_state(board_led_state_t state);

#ifdef __cplusplus
}
#endif

#endif // BOARD_LED_H
