/**
 * @file board_led.c
 * @brief Status indication via main LED strip (first 3 pixels of strip 0)
 *
 * Mirrors the LD2450 project board_led behaviour exactly, using
 * esp_timer for blink and timeout — no polling required.
 */

#include "board_led.h"
#include "led_driver.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "board_led";

#define STATUS_LED_COUNT    3
#define TIMED_STATE_US      (5 * 1000 * 1000)   /* 5 seconds */

static board_led_state_t s_state = BOARD_LED_OFF;
static esp_timer_handle_t s_blink_timer = NULL;
static esp_timer_handle_t s_timeout_timer = NULL;
static bool s_blink_on = false;

static void status_apply(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < STATUS_LED_COUNT; i++) {
        led_driver_set_pixel(0, i, r, g, b, 0);
    }
    led_driver_refresh();
}

static void status_clear(void)
{
    status_apply(0, 0, 0);
}

static void blink_cb(void *arg)
{
    (void)arg;
    s_blink_on = !s_blink_on;

    switch (s_state) {
    case BOARD_LED_NOT_JOINED:
        if (s_blink_on) status_apply(40, 20, 0);   /* amber */
        else status_clear();
        break;
    case BOARD_LED_PAIRING:
        if (s_blink_on) status_apply(0, 0, 40);    /* blue */
        else status_clear();
        break;
    case BOARD_LED_ERROR:
        if (s_blink_on) status_apply(60, 0, 0);    /* red */
        else status_clear();
        break;
    default:
        break;
    }
}

static void timeout_cb(void *arg)
{
    (void)arg;
    switch (s_state) {
    case BOARD_LED_JOINED:
        board_led_set_state(BOARD_LED_OFF);
        break;
    case BOARD_LED_ERROR:
        board_led_set_state(BOARD_LED_PAIRING);
        break;
    default:
        break;
    }
}

void board_led_init(void)
{
    const esp_timer_create_args_t blink_args = {
        .callback = blink_cb,
        .name = "led_blink",
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_args, &s_blink_timer));

    const esp_timer_create_args_t timeout_args = {
        .callback = timeout_cb,
        .name = "led_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timeout_args, &s_timeout_timer));

    ESP_LOGI(TAG, "Status indication on strip 0 (first %d LEDs)", STATUS_LED_COUNT);
}

void board_led_set_state(board_led_state_t state)
{
    s_state = state;
    s_blink_on = false;

    /* Stop any running timers */
    esp_timer_stop(s_blink_timer);
    esp_timer_stop(s_timeout_timer);

    switch (state) {
    case BOARD_LED_OFF:
        status_clear();
        break;

    case BOARD_LED_NOT_JOINED:
        /* blinking amber ~2Hz, indefinite */
        esp_timer_start_periodic(s_blink_timer, 250 * 1000);
        break;

    case BOARD_LED_PAIRING:
        /* blinking blue ~2Hz, indefinite */
        esp_timer_start_periodic(s_blink_timer, 250 * 1000);
        break;

    case BOARD_LED_JOINED:
        /* solid green for 5 s, then OFF */
        status_apply(0, 60, 0);
        esp_timer_start_once(s_timeout_timer, TIMED_STATE_US);
        break;

    case BOARD_LED_ERROR:
        /* blinking red ~5Hz for 5 s, then NOT_JOINED */
        esp_timer_start_periodic(s_blink_timer, 100 * 1000);
        esp_timer_start_once(s_timeout_timer, TIMED_STATE_US);
        break;

    default:
        status_clear();
        break;
    }
}
